namespace EMOTIX360.App.Models;

/// <summary>
/// UI-safe information about a discovered BLE peripheral.
/// The platform-specific Plugin.BLE IDevice is retained inside BluetoothService.
/// </summary>
public sealed class DeviceInfo
{
    public DeviceInfo(Guid id, string name, int rssi)
    {
        Id = id;
        Name = string.IsNullOrWhiteSpace(name) ? "Unknown BLE device" : name;
        Rssi = rssi;
    }

    public Guid Id { get; }
    public string Name { get; }
    public int Rssi { get; }
    public string SignalText => $"RSSI {Rssi} dBm";
}
