using Microsoft.Maui.ApplicationModel;

#if ANDROID
using EMOTIX360.App.Platforms.Android;
#endif

namespace EMOTIX360.App.Services;

public sealed class BluetoothPermissionService
{
    public async Task<bool> EnsurePermissionsAsync()
    {
#if ANDROID

        PermissionStatus status;

        if (OperatingSystem.IsAndroidVersionAtLeast(31))
        {
            status =
                await Permissions.RequestAsync<BluetoothRuntimePermissions>();
        }
        else
        {
            status =
                await Permissions.RequestAsync<Permissions.LocationWhenInUse>();
        }

        return status == PermissionStatus.Granted;

#else

        // iOS requests Bluetooth authorization through CoreBluetooth.
        // The usage description is already present in Info.plist.
        return true;

#endif
    }
}