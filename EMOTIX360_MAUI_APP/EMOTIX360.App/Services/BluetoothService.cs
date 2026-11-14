using System.Diagnostics;
using System.Text;
using Microsoft.Maui.ApplicationModel;
using Plugin.BLE;
using Plugin.BLE.Abstractions;
using Plugin.BLE.Abstractions.Contracts;
using Plugin.BLE.Abstractions.EventArgs;
using ModelDeviceInfo = EMOTIX360.App.Models.DeviceInfo;
using System.Threading;

namespace EMOTIX360.App.Services;

/// <summary>
/// Owns BLE scanning, connection, GATT discovery, status notifications and command writes.
/// UI and view-model code must not retain Plugin.BLE service/characteristic objects.
/// </summary>
public sealed class BluetoothService
{
    public const string DeviceName = "EMOTIX360";

    public static readonly Guid ServiceUuid =
        Guid.Parse("4fafc201-1fb5-459e-8fcc-c5c9c331914b");

    public static readonly Guid CommandCharacteristicUuid =
        Guid.Parse("beb5483e-36e1-4688-b7f5-ea07361b26a8");

    public static readonly Guid StatusCharacteristicUuid =
        Guid.Parse("e3223119-9445-4e96-a4a1-85358c4046a2");

    private readonly IBluetoothLE _bluetoothLe;
    private readonly IAdapter _adapter;
    private readonly object _devicesLock = new();
    private readonly Dictionary<Guid, IDevice> _discoveredDevices = new();
    private readonly SemaphoreSlim _gattSemaphore = new(1, 1);

    private IDevice? _connectedDevice;
    private ICharacteristic? _commandCharacteristic;
    private ICharacteristic? _statusCharacteristic;
    private EventHandler<CharacteristicUpdatedEventArgs>? _statusUpdatedHandler;

    // --------------------------------------------------------
    // BLE CONNECTION WATCHDOG
    // --------------------------------------------------------

    // Firmware normally sends a status notification every 1 second.
    // Six seconds allows several missed notifications before
    // considering the connection lost.

    private static readonly TimeSpan STATUS_TIMEOUT =
        TimeSpan.FromSeconds(6);

    private CancellationTokenSource? _heartbeatMonitorCts;

    private long _lastStatusReceivedUtcTicks;

    public BluetoothService()
    {
        _bluetoothLe = CrossBluetoothLE.Current;
        _adapter = _bluetoothLe.Adapter;

        _adapter.DeviceDiscovered += OnDeviceDiscovered;
        _adapter.DeviceDisconnected += OnDeviceDisconnected;
    }

    public event Action<ModelDeviceInfo>? DeviceDiscovered;
    public event Action<bool, string?>? ConnectionChanged;
    public event Action<string>? StatusPayloadReceived;

    public bool IsBluetoothOn => _bluetoothLe.State == BluetoothState.On;
    public bool IsConnected => _connectedDevice is not null;
    public string? ConnectedDeviceName => _connectedDevice?.Name;
    public BluetoothState BluetoothState => _bluetoothLe.State;

    /// <summary>Requests the runtime permissions required by the current platform.</summary>
    public async Task EnsurePermissionsAsync()
    {
#if ANDROID
        PermissionStatus status = await Permissions.RequestAsync<BluetoothRuntimePermission>();
        if (status != PermissionStatus.Granted)
            throw new UnauthorizedAccessException("Bluetooth scan/connect permissions were not granted.");
#endif
        // iOS displays its Bluetooth permission prompt when CoreBluetooth is first used.
    }

    /// <summary>
    /// Scans for the exact EMOTIX360 advertised name. The firmware also advertises
    /// the service UUID; GATT service validation is performed again after connection.
    /// </summary>
    public async Task ScanAsync(TimeSpan? scanDuration = null, CancellationToken cancellationToken = default)
    {
        await EnsurePermissionsAsync();

        if (!IsBluetoothOn)
            throw new InvalidOperationException("Bluetooth is turned off. Enable Bluetooth and try again.");

        if (_adapter.IsScanning)
            await _adapter.StopScanningForDevicesAsync();

        lock (_devicesLock)
            _discoveredDevices.Clear();

        _adapter.ScanTimeout = (int)(scanDuration ?? TimeSpan.FromSeconds(10)).TotalMilliseconds;

        using CancellationTokenRegistration registration = cancellationToken.Register(
            () => _ = StopScanSafelyAsync());

        try
        {
            await _adapter.StartScanningForDevicesAsync();
        }
        finally
        {
            if (_adapter.IsScanning)
                await _adapter.StopScanningForDevicesAsync();
        }
    }

    public async Task StopScanAsync()
    {
        if (_adapter.IsScanning)
            await _adapter.StopScanningForDevicesAsync();
    }

    public async Task ConnectAsync(Guid deviceId)
    {
        IDevice device;

        lock (_devicesLock)
        {
            if (!_discoveredDevices.TryGetValue(deviceId, out device!))
                throw new InvalidOperationException("The selected device is no longer in the scan results. Scan again.");
        }

        await EnsurePermissionsAsync();
        await StopScanAsync();

        if (_connectedDevice is not null)
            await DisconnectAsync();

        try
        {
            await _adapter.ConnectToDeviceAsync(device);
            _connectedDevice = device;


            // --------------------------------------------------------
            // NEGOTIATE BLE MTU
            // --------------------------------------------------------

            // ESP32-S3 is configured for MTU 100.
            // Request the same MTU from the Android central device.
            //
            // MTU 100 provides up to 97 bytes of notification data.

#if ANDROID
            try
            {
                int negotiatedMtu = await device.RequestMtuAsync(100);

                Debug.WriteLine(
                    $"[BLE] ATT MTU negotiated: {negotiatedMtu}"
                );

                Debug.WriteLine(
                    $"[BLE] Maximum notification payload: " +
                    $"{negotiatedMtu - 3} bytes"
                );
            }
            catch (Exception ex)
            {
                Debug.WriteLine(
                    $"[BLE] MTU negotiation failed: {ex.Message}"
                );

                // Continue connecting, but subsequent notifications
                // might remain limited to the default 20-byte payload.
            }
#endif


            // --------------------------------------------------------
            // DISCOVER GATT SERVICE
            // --------------------------------------------------------

            IService? service =
                await device.GetServiceAsync(ServiceUuid);

            if (service is null)
                throw new InvalidOperationException("EMOTIX360 BLE service was not found on the connected device.");

            _commandCharacteristic = await service.GetCharacteristicAsync(CommandCharacteristicUuid);
            _statusCharacteristic = await service.GetCharacteristicAsync(StatusCharacteristicUuid);

            if (_commandCharacteristic is null)
                throw new InvalidOperationException("The EMOTIX360 command characteristic was not found.");

            if (_statusCharacteristic is null)
                throw new InvalidOperationException("The EMOTIX360 status characteristic was not found.");

            _statusUpdatedHandler = OnStatusCharacteristicUpdated;
            _statusCharacteristic.ValueUpdated += _statusUpdatedHandler;
            await _statusCharacteristic.StartUpdatesAsync();

            // Read the current value once as well as subscribing to future notifications.
            // The firmware may still return "booting" until its status publisher is added.
            try
            {
                var readResult = await _statusCharacteristic.ReadAsync();

                if (readResult.resultCode == 0)
                {
                    PublishStatus(readResult.data);
                }
                else
                {
                    Debug.WriteLine(
                        $"[BLE] Initial status read failed. " +
                        $"GATT result: {readResult.resultCode}"
                    );
                }
            }
            catch (Exception ex)
            {
                // Keep the GATT connection alive if the initial read fails.
                // Notifications may still deliver the next valid status packet.

                Debug.WriteLine(
                    $"[BLE] Initial status read exception: {ex.Message}"
                );
            }

            ConnectionChanged?.Invoke(true, device.Name);
            // Start monitoring incoming ESP32 status notifications.
            StartHeartbeatMonitor();
        }
        catch
        {
            await DisconnectAfterFailedConnectAsync(device);
            throw;
        }
    }

    public async Task DisconnectAsync()
    {
        // Serialize disconnect with writes; Android GATT operations should not overlap.
        await _gattSemaphore.WaitAsync();
        try
        {
            IDevice? device = _connectedDevice;
            if (device is null)
                return;

            try
            {
                ICharacteristic? statusCharacteristic = _statusCharacteristic;
                EventHandler<CharacteristicUpdatedEventArgs>? statusHandler = _statusUpdatedHandler;

                if (statusCharacteristic is not null)
                {
                    if (statusHandler is not null)
                        statusCharacteristic.ValueUpdated -= statusHandler;

                    try
                    {
                        await statusCharacteristic.StopUpdatesAsync();
                    }
                    catch
                    {
                        // A lost link may already have stopped notifications.
                        // Continue and explicitly ask the adapter to disconnect.
                    }
                }

                await _adapter.DisconnectDeviceAsync(device);
            }
            finally
            {
                // DeviceDisconnected may already have cleared the state and raised the event.
                bool stillOwned = _connectedDevice?.Id == device.Id;
                ClearConnectionState();

                if (stillOwned)
                    ConnectionChanged?.Invoke(false, "Disconnected");
            }
        }
        finally
        {
            _gattSemaphore.Release();
        }
    }

    /// <summary>
    /// Writes a UTF-8 command without a newline. Android GATT operations are serialized
    /// because overlapping characteristic operations can fail at the platform layer.
    /// </summary>
    public async Task WriteCommandAsync(string command)
    {
        if (string.IsNullOrWhiteSpace(command))
        {
            throw new ArgumentException(
                "Command cannot be empty.",
                nameof(command)
            );
        }

        byte[] payload = Encoding.UTF8.GetBytes(command.Trim());

        await _gattSemaphore.WaitAsync();

        try
        {
            ICharacteristic characteristic =
                _commandCharacteristic
                ?? throw new InvalidOperationException(
                    "Not connected to EMOTIX360 command characteristic."
                );

            if (!IsConnected)
            {
                throw new InvalidOperationException(
                    "EMOTIX360 is not connected."
                );
            }

            // Plugin.BLE returns an integer GATT result code.
            // Keep the write on the Android/iOS main thread.
            int resultCode = await MainThread.InvokeOnMainThreadAsync(
                () => characteristic.WriteAsync(payload)
            );

            // GATT_SUCCESS = 0.
            if (resultCode != 0)
            {
                throw new IOException(
                    $"BLE command write failed. GATT result code: {resultCode}"
                );
            }
        }
        finally
        {
            _gattSemaphore.Release();
        }
    }

    private void OnDeviceDiscovered(object? sender, DeviceEventArgs args)
    {
        IDevice device = args.Device;
        if (!string.Equals(device.Name, DeviceName, StringComparison.OrdinalIgnoreCase))
            return;

        lock (_devicesLock)
            _discoveredDevices[device.Id] = device;

        DeviceDiscovered?.Invoke(
            new ModelDeviceInfo(
                device.Id,
                device.Name,
                device.Rssi
            )
        );
    }

    private void OnDeviceDisconnected(
    object? sender,
    DeviceEventArgs args)
    {
        if (_connectedDevice?.Id != args.Device.Id)
            return;

        // Remove the status notification handler from the
        // disconnected characteristic.
        if (_statusCharacteristic is not null &&
            _statusUpdatedHandler is not null)
        {
            _statusCharacteristic.ValueUpdated -=
                _statusUpdatedHandler;
        }

        ClearConnectionState();

        ConnectionChanged?.Invoke(
            false,
            "The device disconnected."
        );
    }

    private void OnStatusCharacteristicUpdated(object? sender, CharacteristicUpdatedEventArgs args)
    {
        PublishStatus(args.Characteristic.Value);
    }

    private void PublishStatus(byte[]? bytes)
    {
        if (bytes is null || bytes.Length == 0)
            return;

        // Record that the ESP32 is still communicating.
        Interlocked.Exchange(
            ref _lastStatusReceivedUtcTicks,
            DateTime.UtcNow.Ticks
        );

        string payload = Encoding.UTF8.GetString(bytes)
            .Trim('\0', '\r', '\n', ' ');

        if (!string.IsNullOrWhiteSpace(payload))
        {
            StatusPayloadReceived?.Invoke(payload);
        }
    }

    private async Task DisconnectAfterFailedConnectAsync(IDevice device)
    {
        try
        {
            if (_statusCharacteristic is not null && _statusUpdatedHandler is not null)
                _statusCharacteristic.ValueUpdated -= _statusUpdatedHandler;

            await _adapter.DisconnectDeviceAsync(device);
        }
        catch
        {
            // Preserve the original service-discovery/connection exception.
        }
        finally
        {
            bool stillOwned = _connectedDevice?.Id == device.Id;
            ClearConnectionState();

            if (stillOwned)
                ConnectionChanged?.Invoke(false, "Connection failed.");
        }
    }


    // ========================================================
    // BLE CONNECTION WATCHDOG
    // ========================================================

    private void StartHeartbeatMonitor()
    {
        StopHeartbeatMonitor();

        Interlocked.Exchange(
            ref _lastStatusReceivedUtcTicks,
            DateTime.UtcNow.Ticks
        );

        var source = new CancellationTokenSource();

        Interlocked.Exchange(
            ref _heartbeatMonitorCts,
            source
        );

        _ = MonitorHeartbeatAsync(source);
    }


    private void StopHeartbeatMonitor()
    {
        var source = Interlocked.Exchange(
            ref _heartbeatMonitorCts,
            null
        );

        if (source is not null)
        {
            try
            {
                source.Cancel();
            }
            catch (ObjectDisposedException)
            {
                // Monitor already finished.
            }
        }
    }


    // --------------------------------------------------------
    // MONITOR INCOMING STATUS
    // --------------------------------------------------------

    private async Task MonitorHeartbeatAsync(
        CancellationTokenSource source)
    {
        try
        {
            using var timer = new PeriodicTimer(
                TimeSpan.FromSeconds(1)
            );

            while (await timer.WaitForNextTickAsync(source.Token))
            {
                // No active connection.
                if (_connectedDevice is null)
                    return;

                long lastReceivedTicks =
                    Interlocked.Read(
                        ref _lastStatusReceivedUtcTicks
                    );

                if (lastReceivedTicks == 0)
                    continue;

                long elapsedTicks =
                    DateTime.UtcNow.Ticks - lastReceivedTicks;

                if (elapsedTicks >= STATUS_TIMEOUT.Ticks)
                {
                    Debug.WriteLine(
                        "[BLE] Status heartbeat timeout. " +
                        "Connection considered lost."
                    );

                    await HandleHeartbeatTimeoutAsync(source.Token);

                    return;
                }
            }
        }
        catch (OperationCanceledException)
        {
            // Expected during a manual or unexpected disconnect.
        }
        catch (Exception ex)
        {
            Debug.WriteLine(
                $"[BLE] Heartbeat monitor error: {ex.Message}"
            );
        }
        finally
        {
            Interlocked.CompareExchange(
                ref _heartbeatMonitorCts,
                null,
                source
            );

            source.Dispose();
        }
    }


    // --------------------------------------------------------
    // HANDLE HEARTBEAT TIMEOUT
    // --------------------------------------------------------

    private async Task HandleHeartbeatTimeoutAsync(
        CancellationToken cancellationToken)
    {
        IDevice? lostDevice = null;

        await _gattSemaphore.WaitAsync(cancellationToken);

        try
        {
            IDevice? device = _connectedDevice;

            if (device is null)
                return;

            // Recheck before clearing the connection.
            long lastReceivedTicks =
                Interlocked.Read(
                    ref _lastStatusReceivedUtcTicks
                );

            if (lastReceivedTicks != 0 &&
                DateTime.UtcNow.Ticks - lastReceivedTicks
                    < STATUS_TIMEOUT.Ticks)
            {
                return;
            }

            lostDevice = device;

            // Remove the notification callback.
            if (_statusCharacteristic is not null &&
                _statusUpdatedHandler is not null)
            {
                _statusCharacteristic.ValueUpdated -=
                    _statusUpdatedHandler;
            }

            // Clear all local connection references.
            ClearConnectionState();

            Debug.WriteLine(
                "[BLE] EMOTIX360 connection lost."
            );

            // Notify HomeViewModel and DeviceViewModel.
            ConnectionChanged?.Invoke(
                false,
                "EMOTIX360 disconnected. Device is powered off or out of range."
            );
        }
        finally
        {
            _gattSemaphore.Release();
        }

        // Request cleanup of the stale peripheral connection.
        // This is best-effort because the device may already be off.
        if (lostDevice is not null)
        {
            try
            {
                await _adapter.DisconnectDeviceAsync(lostDevice);
            }
            catch (Exception ex)
            {
                Debug.WriteLine(
                    $"[BLE] Disconnection cleanup: {ex.Message}"
                );
            }
        }
    }


    private void ClearConnectionState()
    {
        StopHeartbeatMonitor();

        Interlocked.Exchange(
            ref _lastStatusReceivedUtcTicks,
            0
        );

        _connectedDevice = null;
        _commandCharacteristic = null;
        _statusCharacteristic = null;
        _statusUpdatedHandler = null;
    }

    private async Task StopScanSafelyAsync()
    {
        try
        {
            await StopScanAsync();
        }
        catch
        {
            // Cancellation may race with the scan timeout; ignore only this cleanup error.
        }
    }
}

#if ANDROID
/// <summary>Android 12+: Nearby devices permissions. Android 11 and earlier: location for BLE scanning.</summary>
internal sealed class BluetoothRuntimePermission : Permissions.BasePlatformPermission
{
    public override (string androidPermission, bool isRuntime)[] RequiredPermissions
    {
        get
        {
            if (OperatingSystem.IsAndroidVersionAtLeast(31))
            {
                return new[]
                {
                    (Android.Manifest.Permission.BluetoothScan, true),
                    (Android.Manifest.Permission.BluetoothConnect, true)
                };
            }

            return new[]
            {
                (Android.Manifest.Permission.AccessFineLocation, true)
            };
        }
    }
}
#endif
