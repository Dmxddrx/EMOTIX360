using System;
using System.Collections.Generic;
using System.Text;
using Microsoft.Maui.ApplicationModel;

namespace EMOTIX360.App.Platforms.Android
{
    public sealed class BluetoothRuntimePermissions
    : Permissions.BasePlatformPermission
    {
        public override (string androidPermission, bool isRuntime)[]
            RequiredPermissions =>
            [
                ("android.permission.BLUETOOTH_SCAN", true),
            ("android.permission.BLUETOOTH_CONNECT", true)
            ];
    }
}
