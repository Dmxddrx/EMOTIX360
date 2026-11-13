using System.Windows.Input;
using Microsoft.Maui.ApplicationModel;
using Microsoft.Maui.Controls;
using EMOTIX360.App.Models;
using EMOTIX360.App.Services;
using Microsoft.Maui.Graphics;

namespace EMOTIX360.App.ViewModels;

public sealed class HomeViewModel : ObservableObject
{
    private readonly BluetoothService _bluetooth;
    private readonly DeviceCommandService _commands;
    private readonly DeviceStatusService _statusService;

    private bool _isConnected;
    private string _dashboardMessage = "Connect your EMOTIX360 to see live device data.";
    private string _activeModeText = "--";
    private string _brightnessText = "--";
    private string _wifiText = "--";
    private string _motionText = "--";
    private double _brightnessValue = 5;
    private string _timeText = "--";

    public HomeViewModel(
        BluetoothService bluetooth,
        DeviceCommandService commands,
        DeviceStatusService statusService)
    {
        _bluetooth = bluetooth;
        _commands = commands;
        _statusService = statusService;

        OpenDeviceCommand = new Command(async () => await Shell.Current.GoToAsync("//device"));
        SelectAnimationCommand = new Command<string>(
            async mode => await SelectAnimationAsync(mode),
            _ => IsConnected);
        ApplyBrightnessCommand = new Command(
            async () => await ApplyBrightnessAsync(),
            () => IsConnected);

        _bluetooth.ConnectionChanged += OnConnectionChanged;
        _statusService.StatusUpdated += OnStatusUpdated;
        _statusService.StatusCleared += OnStatusCleared;

        IsConnected = _bluetooth.IsConnected;
        if (_statusService.Current is not null)
            ApplyStatus(_statusService.Current);
    }

    public ICommand OpenDeviceCommand { get; }
    public Command<string> SelectAnimationCommand { get; }
    public Command ApplyBrightnessCommand { get; }

    public bool IsConnected
    {
        get => _isConnected;
        private set
        {
            if (!SetProperty(ref _isConnected, value))
                return;

            OnPropertyChanged(nameof(ConnectionText));
            OnPropertyChanged(nameof(ConnectionDescription));
            OnPropertyChanged(nameof(ConnectionIndicatorColor));
            OnPropertyChanged(nameof(ConnectButtonText));
            OnPropertyChanged(nameof(OverviewStatusText));
            SelectAnimationCommand.ChangeCanExecute();
            ApplyBrightnessCommand.ChangeCanExecute();
        }
    }

    public string ConnectionText => IsConnected ? "CONNECTED" : "OFFLINE";
    public string ConnectionDescription => IsConnected ? "Connected to EMOTIX360" : "Not connected";
    public Color ConnectionIndicatorColor => IsConnected
        ? Color.FromArgb("#72C9A0")
        : Color.FromArgb("#F2A65A");
    public string ConnectButtonText => IsConnected ? "DEVICE CONNECTED" : "CONNECT DEVICE";
    public string OverviewStatusText => IsConnected ? "LIVE DEVICE DATA" : "AWAITING CONNECTION";

    public string DashboardMessage
    {
        get => _dashboardMessage;
        private set => SetProperty(ref _dashboardMessage, value);
    }

    public string ActiveModeText
    {
        get => _activeModeText;
        private set => SetProperty(ref _activeModeText, value);
    }

    public string BrightnessText
    {
        get => _brightnessText;
        private set => SetProperty(ref _brightnessText, value);
    }

    public string WifiText
    {
        get => _wifiText;
        private set => SetProperty(ref _wifiText, value);
    }

    public string MotionText
    {
        get => _motionText;
        private set => SetProperty(ref _motionText, value);
    }

    /// <summary>0..15 MAX7219 intensity. The user applies it with the Apply button.</summary>
    public double BrightnessValue
    {
        get => _brightnessValue;
        set
        {
            double clamped = Math.Clamp(value, 0, 15);
            if (SetProperty(ref _brightnessValue, clamped))
                OnPropertyChanged(nameof(BrightnessSliderText));
        }
    }

    public string TimeText
    {
        get => _timeText;
        private set => SetProperty(ref _timeText, value);
    }

    public string BrightnessSliderText => $"{(int)Math.Round(BrightnessValue)} / 15";

    private async Task SelectAnimationAsync(string? wireName)
    {
        if (!IsConnected)
        {
            DashboardMessage = "Connect to EMOTIX360 before selecting an animation.";
            return;
        }

        if (!AnimationModeExtensions.TryParseFirmwareValue(wireName, out AnimationMode mode))
        {
            DashboardMessage = "Unknown animation mode.";
            return;
        }

        try
        {
            await _commands.SetModeAsync(mode);
            DashboardMessage = $"Sent {mode.ToDisplayName()} command. Waiting for device status.";
        }
        catch (Exception ex)
        {
            DashboardMessage = $"Could not send animation command: {ex.Message}";
        }
    }

    private async Task ApplyBrightnessAsync()
    {
        if (!IsConnected)
        {
            DashboardMessage = "Connect to EMOTIX360 before changing brightness.";
            return;
        }

        int value = (int)Math.Round(BrightnessValue);
        try
        {
            await _commands.SetBrightnessAsync(value);
            DashboardMessage = $"Sent brightness {value}. Waiting for device status.";
        }
        catch (Exception ex)
        {
            DashboardMessage = $"Could not send brightness command: {ex.Message}";
        }
    }

    private void OnConnectionChanged(bool connected, string? detail)
    {
        MainThread.BeginInvokeOnMainThread(() =>
        {
            IsConnected = connected;
            DashboardMessage = connected
                ? $"Connected to {detail ?? BluetoothService.DeviceName}. Waiting for telemetry."
                : detail ?? "EMOTIX360 is disconnected.";

            if (!connected)
            {
                ActiveModeText = "--";
                BrightnessText = "--";
                WifiText = "--";
                MotionText = "--";
                TimeText = "--";
            }
        });
    }

    private void OnStatusUpdated(DeviceStatus status)
    {
        MainThread.BeginInvokeOnMainThread(() => ApplyStatus(status));
    }

    private void OnStatusCleared()
    {
        MainThread.BeginInvokeOnMainThread(() =>
        {
            ActiveModeText = "--";
            BrightnessText = "--";
            WifiText = "--";
            MotionText = "--";
            TimeText = "--";
        });
    }

    private void ApplyStatus(DeviceStatus status)
    {
        if (!string.IsNullOrWhiteSpace(status.Mode))
            ActiveModeText = status.Mode.Replace('_', ' ');

        if (status.Brightness.HasValue)
        {
            BrightnessText = status.Brightness.Value.ToString();
            BrightnessValue = status.Brightness.Value;
        }

        if (status.WifiConnected.HasValue)
            WifiText = status.WifiConnected.Value ? "ONLINE" : "OFFLINE";

        // Display actual MPU6050 orientation.
        if (status.PitchDegrees.HasValue &&
            status.RollDegrees.HasValue)
        {
            MotionText =
                $"P {status.PitchDegrees.Value:+0.0;-0.0;0.0}°\n" +
                $"R {status.RollDegrees.Value:+0.0;-0.0;0.0}°";
        }
        else
        {
            MotionText = "NO DATA";
        }

        // Display NTP synchronization status.
        if (status.TimeValid.HasValue)
        {
            TimeText = status.TimeValid.Value
                ? "NTP SYNCHRONIZED"
                : "NOT SYNCHRONIZED";
        }

        if (IsConnected)
            DashboardMessage = $"Last device update: {status.ReceivedAt.ToLocalTime():HH:mm:ss}";
    }
}
