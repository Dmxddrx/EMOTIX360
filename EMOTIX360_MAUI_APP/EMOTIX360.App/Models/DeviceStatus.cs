using System.Globalization;

namespace EMOTIX360.App.Models;

/// <summary>
/// Parsed snapshot published by the ESP32-S3 status characteristic.
/// Example: P=12.3,R=-4.5,WIFI=1,TIME=1,MODE=HEART,BRIGHT=5
/// </summary>
public sealed class DeviceStatus
{
    public float? PitchDegrees { get; init; }
    public float? RollDegrees { get; init; }
    public bool? WifiConnected { get; init; }
    public bool? TimeValid { get; init; }
    public string? Mode { get; init; }
    public int? Brightness { get; init; }
    public DateTimeOffset ReceivedAt { get; init; } = DateTimeOffset.Now;

    public static bool TryParse(string? payload, out DeviceStatus status)
    {
        status = new DeviceStatus();

        if (string.IsNullOrWhiteSpace(payload))
            return false;

        float? pitch = null;
        float? roll = null;
        bool? wifi = null;
        bool? time = null;
        string? mode = null;
        int? brightness = null;
        bool foundKnownField = false;

        foreach (string item in payload.Trim().Split(',', StringSplitOptions.RemoveEmptyEntries))
        {
            string[] pair = item.Split('=', 2, StringSplitOptions.TrimEntries);
            if (pair.Length != 2)
                continue;

            string key = pair[0].Trim().ToUpperInvariant();
            string value = pair[1].Trim();

            switch (key)
            {
                case "P":
                    if (float.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out float p))
                    {
                        pitch = p;
                        foundKnownField = true;
                    }
                    break;

                case "R":
                    if (float.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out float r))
                    {
                        roll = r;
                        foundKnownField = true;
                    }
                    break;

                case "WIFI":
                    if (TryParseFlag(value, out bool wifiValue))
                    {
                        wifi = wifiValue;
                        foundKnownField = true;
                    }
                    break;

                case "TIME":
                    if (TryParseFlag(value, out bool timeValue))
                    {
                        time = timeValue;
                        foundKnownField = true;
                    }
                    break;

                case "MODE":
                    if (!string.IsNullOrWhiteSpace(value))
                    {
                        mode = value.ToUpperInvariant();
                        foundKnownField = true;
                    }
                    break;

                case "BRIGHT":
                    if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out int bright)
                        && bright is >= 0 and <= 15)
                    {
                        brightness = bright;
                        foundKnownField = true;
                    }
                    break;
            }
        }

        if (!foundKnownField)
            return false;

        status = new DeviceStatus
        {
            PitchDegrees = pitch,
            RollDegrees = roll,
            WifiConnected = wifi,
            TimeValid = time,
            Mode = mode,
            Brightness = brightness,
            ReceivedAt = DateTimeOffset.Now
        };

        return true;
    }

    private static bool TryParseFlag(string value, out bool result)
    {
        result = false;
        if (value == "1")
        {
            result = true;
            return true;
        }

        return value == "0";
    }
}
