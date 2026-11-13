using EMOTIX360.App.Models;

namespace EMOTIX360.App.Services;

/// <summary>
/// Validates and formats the text protocol expected by the EMOTIX360 firmware.
/// These are the documented/proposed wire commands; firmware must consume takeCommand()
/// before the physical animation or brightness can change.
/// </summary>
public sealed class DeviceCommandService
{
    private readonly BluetoothService _bluetooth;

    public DeviceCommandService(BluetoothService bluetooth)
    {
        _bluetooth = bluetooth;
    }

    public Task SetModeAsync(AnimationMode mode)
    {
        string command = $"MODE={mode.ToFirmwareValue()}";
        return _bluetooth.WriteCommandAsync(command);
    }

    public Task SetBrightnessAsync(int brightness)
    {
        if (brightness is < 0 or > 15)
            throw new ArgumentOutOfRangeException(nameof(brightness), "MAX7219 brightness must be between 0 and 15.");

        return _bluetooth.WriteCommandAsync($"BRIGHT={brightness}");
    }
}
