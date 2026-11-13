namespace EMOTIX360.App.Models;

/// <summary>
/// Animation modes that can be selected by the mobile application.
/// STARTUP is intentionally excluded because it is a one-time firmware boot effect.
/// </summary>
public enum AnimationMode
{
    Idle,
    SandClock,
    Water,
    SandGravity,
    Heart,
    Rain,
    Waves
}

public static class AnimationModeExtensions
{
    public static string ToFirmwareValue(this AnimationMode mode) => mode switch
    {
        AnimationMode.Idle => "IDLE",
        AnimationMode.SandClock => "SAND_CLOCK",
        AnimationMode.Water => "WATER",
        AnimationMode.SandGravity => "SAND_GRAVITY",
        AnimationMode.Heart => "HEART",
        AnimationMode.Rain => "RAIN",
        AnimationMode.Waves => "WAVES",
        _ => throw new ArgumentOutOfRangeException(nameof(mode), mode, "Unsupported animation mode.")
    };

    public static string ToDisplayName(this AnimationMode mode) => mode switch
    {
        AnimationMode.Idle => "IDLE",
        AnimationMode.SandClock => "SAND CLOCK",
        AnimationMode.Water => "WATER",
        AnimationMode.SandGravity => "SAND GRAVITY",
        AnimationMode.Heart => "HEART",
        AnimationMode.Rain => "RAIN",
        AnimationMode.Waves => "WAVES",
        _ => "UNKNOWN"
    };

    public static bool TryParseFirmwareValue(string? value, out AnimationMode mode)
    {
        mode = AnimationMode.Idle;

        if (string.IsNullOrWhiteSpace(value))
            return false;

        mode = value.Trim().ToUpperInvariant() switch
        {
            "IDLE" => AnimationMode.Idle,
            "SAND_CLOCK" => AnimationMode.SandClock,
            "WATER" => AnimationMode.Water,
            "SAND_GRAVITY" => AnimationMode.SandGravity,
            "HEART" => AnimationMode.Heart,
            "RAIN" => AnimationMode.Rain,
            "WAVES" => AnimationMode.Waves,
            _ => (AnimationMode)(-1)
        };

        return Enum.IsDefined(mode);
    }
}
