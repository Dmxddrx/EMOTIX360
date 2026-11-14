
using System.Globalization;

namespace EMOTIX360.App.Models;

/// <summary>
/// Complete device status snapshot published by ESP32-S3.
///
/// Expected format:
/// P=12.3,R=-4.5,WIFI=1,TIME=1,MODE=HEART,BRIGHT=5
///
/// Incomplete or truncated notifications are rejected.
/// </summary>
public sealed class DeviceStatus
{
    public float? PitchDegrees { get; init; }

    public float? RollDegrees { get; init; }

    public bool? WifiConnected { get; init; }

    public bool? TimeValid { get; init; }

    public string? Mode { get; init; }

    public int? Brightness { get; init; }

    public DateTimeOffset ReceivedAt { get; init; }
        = DateTimeOffset.Now;


    // ========================================================
    // STATUS PARSER
    // ========================================================

    public static bool TryParse(
        string? payload,
        out DeviceStatus status)
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


        // ----------------------------------------------------
        // PARSE INDIVIDUAL FIELDS
        // ----------------------------------------------------

        foreach (string item in payload
                     .Trim()
                     .Split(',', StringSplitOptions.RemoveEmptyEntries))
        {
            string[] pair = item.Split(
                '=',
                2,
                StringSplitOptions.TrimEntries
            );

            if (pair.Length != 2)
                continue;

            string key =
                pair[0].Trim().ToUpperInvariant();

            string value = pair[1].Trim();


            switch (key)
            {
                case "P":

                    if (float.TryParse(
                            value,
                            NumberStyles.Float,
                            CultureInfo.InvariantCulture,
                            out float p))
                    {
                        pitch = p;
                    }

                    break;


                case "R":

                    if (float.TryParse(
                            value,
                            NumberStyles.Float,
                            CultureInfo.InvariantCulture,
                            out float r))
                    {
                        roll = r;
                    }

                    break;


                case "WIFI":

                    if (TryParseFlag(value, out bool wifiValue))
                    {
                        wifi = wifiValue;
                    }

                    break;


                case "TIME":

                    if (TryParseFlag(value, out bool timeValue))
                    {
                        time = timeValue;
                    }

                    break;


                case "MODE":

                    if (!string.IsNullOrWhiteSpace(value))
                    {
                        mode = value.ToUpperInvariant();
                    }

                    break;


                case "BRIGHT":

                    if (int.TryParse(
                            value,
                            NumberStyles.Integer,
                            CultureInfo.InvariantCulture,
                            out int bright)
                        && bright is >= 0 and <= 15)
                    {
                        brightness = bright;
                    }

                    break;
            }
        }


        // ----------------------------------------------------
        // VALIDATE COMPLETE STATUS
        // ----------------------------------------------------

        /*
         * All six fields are required.
         *
         * This prevents a truncated 20-byte BLE notification
         * from replacing a previously valid status snapshot.
         */

        if (!pitch.HasValue ||
            !roll.HasValue ||
            !wifi.HasValue ||
            !time.HasValue ||
            string.IsNullOrWhiteSpace(mode) ||
            !brightness.HasValue)
        {
            return false;
        }


        // ----------------------------------------------------
        // CREATE COMPLETE STATUS SNAPSHOT
        // ----------------------------------------------------

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


    // ========================================================
    // BOOLEAN PARSER
    // ========================================================

    private static bool TryParseFlag(
        string value,
        out bool result)
    {
        result = false;

        if (value == "1")
        {
            result = true;
            return true;
        }

        if (value == "0")
        {
            result = false;
            return true;
        }

        return false;
    }
}
