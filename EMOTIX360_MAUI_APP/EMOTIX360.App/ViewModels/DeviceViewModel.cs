using System.Collections.ObjectModel;
using System.Windows.Input;
using Microsoft.Maui.ApplicationModel;
using Microsoft.Maui.Controls;
using ModelDeviceInfo = EMOTIX360.App.Models.DeviceInfo;
using EMOTIX360.App.Services;

namespace EMOTIX360.App.ViewModels;

public sealed class DeviceViewModel : ObservableObject
{
    private readonly BluetoothService _bluetooth;
    private bool _isScanning;
    private bool _isConnecting;
    private bool _isConnected;
    private ModelDeviceInfo? _selectedDevice;
    private string _statusMessage = "Scan to find your nearby EMOTIX360.";

    public DeviceViewModel(BluetoothService bluetooth)
    {
        _bluetooth = bluetooth;

        ScanCommand = new Command(async () => await ScanAsync(), () => CanScan);
        ConnectSelectedCommand = new Command(async () => await ConnectSelectedAsync(), () => CanConnectSelected);
        DisconnectCommand = new Command(async () => await DisconnectAsync(), () => IsConnected);

        _bluetooth.DeviceDiscovered += OnDeviceDiscovered;
        _bluetooth.ConnectionChanged += OnConnectionChanged;
        IsConnected = _bluetooth.IsConnected;
    }

    public ObservableCollection<ModelDeviceInfo> Devices { get; } = new();
    public Command ScanCommand { get; }
    public Command ConnectSelectedCommand { get; }
    public Command DisconnectCommand { get; }

    public bool IsScanning
    {
        get => _isScanning;
        private set
        {
            if (SetProperty(ref _isScanning, value))
            {
                OnPropertyChanged(nameof(ScanButtonText));
                RefreshCommandStates();
            }
        }
    }

    public bool IsConnecting
    {
        get => _isConnecting;
        private set
        {
            if (SetProperty(ref _isConnecting, value))
                RefreshCommandStates();
        }
    }

    public bool IsConnected
    {
        get => _isConnected;
        private set
        {
            if (SetProperty(ref _isConnected, value))
            {
                OnPropertyChanged(nameof(ConnectionText));
                RefreshCommandStates();
            }
        }
    }

    public ModelDeviceInfo? SelectedDevice
    {
        get => _selectedDevice;
        set
        {
            if (SetProperty(ref _selectedDevice, value))
                RefreshCommandStates();
        }
    }

    public string StatusMessage
    {
        get => _statusMessage;
        private set => SetProperty(ref _statusMessage, value);
    }

    public string ConnectionText => IsConnected ? "Connected" : "Not connected";
    public string ScanButtonText => IsScanning ? "SCANNING..." : "SCAN FOR EMOTIX360";
    public bool CanScan => !IsScanning && !IsConnecting && !IsConnected;
    public bool CanConnectSelected => SelectedDevice is not null && !IsScanning && !IsConnecting && !IsConnected;

    private async Task ScanAsync()
    {
        if (!CanScan)
            return;

        Devices.Clear();
        SelectedDevice = null;
        IsScanning = true;
        StatusMessage = "Searching for EMOTIX360 (10 seconds)...";

        try
        {
            await _bluetooth.ScanAsync(TimeSpan.FromSeconds(10));
            StatusMessage = Devices.Count > 0
                ? $"Found {Devices.Count} EMOTIX360 device(s). Select one to connect."
                : "No EMOTIX360 device found. Check that the ESP32-S3 is powered on and advertising.";
        }
        catch (UnauthorizedAccessException ex)
        {
            StatusMessage = ex.Message;
        }
        catch (Exception ex)
        {
            StatusMessage = $"Bluetooth scan failed: {ex.Message}";
        }
        finally
        {
            IsScanning = false;
        }
    }

    private async Task ConnectSelectedAsync()
    {
        if (SelectedDevice is null || !CanConnectSelected)
            return;

        IsConnecting = true;
        StatusMessage = $"Connecting to {SelectedDevice.Name}...";

        try
        {
            await _bluetooth.ConnectAsync(SelectedDevice.Id);
            IsConnected = true;
            StatusMessage = "Connected. GATT service and characteristics discovered; status notifications enabled.";
        }
        catch (Exception ex)
        {
            IsConnected = false;
            StatusMessage = $"Connection failed: {ex.Message}";
        }
        finally
        {
            IsConnecting = false;
        }
    }

    private async Task DisconnectAsync()
    {
        try
        {
            await _bluetooth.DisconnectAsync();
            StatusMessage = "Disconnected from EMOTIX360.";
        }
        catch (Exception ex)
        {
            StatusMessage = $"Disconnect error: {ex.Message}";
        }
    }

    private void OnDeviceDiscovered(ModelDeviceInfo device)
    {
        MainThread.BeginInvokeOnMainThread(() =>
        {
            int existingIndex = -1;
            for (int i = 0; i < Devices.Count; i++)
            {
                if (Devices[i].Id == device.Id)
                {
                    existingIndex = i;
                    break;
                }
            }

            if (existingIndex >= 0)
                Devices[existingIndex] = device;
            else
                Devices.Add(device);
        });
    }

    private void OnConnectionChanged(bool connected, string? detail)
    {
        MainThread.BeginInvokeOnMainThread(() =>
        {
            IsConnected = connected;
            StatusMessage = connected
                ? $"Connected to {detail ?? BluetoothService.DeviceName}."
                : detail ?? "Device disconnected.";
        });
    }

    private void RefreshCommandStates()
    {
        OnPropertyChanged(nameof(CanScan));
        OnPropertyChanged(nameof(CanConnectSelected));
        ScanCommand?.ChangeCanExecute();
        ConnectSelectedCommand?.ChangeCanExecute();
        DisconnectCommand?.ChangeCanExecute();
    }
}
