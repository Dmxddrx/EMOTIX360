using EMOTIX360.App.Models;
using System.Diagnostics;

namespace EMOTIX360.App.Services;

/// <summary>Subscribes to BLE status payloads and exposes parsed device snapshots.</summary>
public sealed class DeviceStatusService
{
    private readonly BluetoothService _bluetooth;

    public DeviceStatusService(BluetoothService bluetooth)
    {
        _bluetooth = bluetooth;
        _bluetooth.StatusPayloadReceived += OnStatusPayloadReceived;
        _bluetooth.ConnectionChanged += OnConnectionChanged;
    }

    public DeviceStatus? Current { get; private set; }

    public event Action<DeviceStatus>? StatusUpdated;
    public event Action? StatusCleared;
    public event Action<string>? UnparsedPayloadReceived;

    private void OnStatusPayloadReceived(string payload)
    {
        Debug.WriteLine(
            $"[EMOTIX360 BLE] Status received: {payload}"
        );

        if (!DeviceStatus.TryParse(payload, out DeviceStatus status))
        {
            Debug.WriteLine(
                "[EMOTIX360 BLE] Status parsing failed."
            );

            UnparsedPayloadReceived?.Invoke(payload);
            return;
        }

        Current = status;

        Debug.WriteLine(
            $"[EMOTIX360 BLE] Parsed MODE={status.Mode}, " +
            $"BRIGHT={status.Brightness}, WIFI={status.WifiConnected}"
        );

        StatusUpdated?.Invoke(status);
    }

    private void OnConnectionChanged(bool connected, string? detail)
    {
        if (connected)
            return;

        Current = null;
        StatusCleared?.Invoke();
    }
}
