# EMOTIX360 MAUI Companion App

The `EMOTIX360_MAUI` project is the mobile companion application for the EMOTIX360 ESP32-S3 device. It is built with **.NET MAUI and C#**, with a shared project targeting Android and iOS.

The app is intended to connect directly to the device over Bluetooth Low Energy (BLE), allowing the user to select animations, adjust matrix brightness, and monitor device status.

> **Current status:** This is the initial application/UI foundation. The dark dashboard and platform BLE permission declarations are included. BLE scanning, connecting, GATT discovery, command writes, and status notifications are not implemented yet. The ESP32 firmware command dispatcher is also a separate remaining integration task.

---

## Project targets

The app currently targets:

```xml
<TargetFrameworks>net10.0-android;net10.0-ios</TargetFrameworks>
```

| Platform | Development status |
|---|---|
| Android | Primary development and BLE test target on Windows. |
| iOS | Target is included; build/deployment requires a compatible Mac with Xcode paired to Visual Studio. |

The project application identity is currently:

- Display name: `EMOTIX360`
- Application ID: `com.dmxddrx.emotix360`
- Initial application version: `1.0` / build `1`

Update the application ID before public distribution if a different package/bundle identifier is required.

---

## Current functionality

The starter project contains:

- Dark-themed EMOTIX360 dashboard shell.
- Device-disconnected presentation state.
- UI previews for the seven firmware modes: IDLE, SAND_CLOCK, WATER, SAND_GRAVITY, HEART, RAIN, and WAVES.
- Placeholder cards for network, time, and motion status.
- A brightness-control preview that is not yet connected to the ESP32.
- Android BLE permission declarations.
- iOS Bluetooth usage-description string.
- Shared .NET MAUI project structure for Android and iOS.

The dashboard values are placeholders until the BLE client and status parser are implemented. The current app does not yet control the physical device.

---

## Development requirements

- Visual Studio 2026.
- The **.NET Multi-platform App UI development** workload.
- .NET 10 SDK and the corresponding MAUI workload components.
- Android SDK and Android build tools supplied/configured through Visual Studio.
- Microsoft OpenJDK component installed by the MAUI workload, unless a compatible JDK is configured separately.
- A physical Android phone for final BLE testing.

For iOS development:

- A Mac compatible with the installed Visual Studio and .NET MAUI toolchain.
- A supported Xcode version installed on the Mac.
- Visual Studio paired to that Mac through **Tools → iOS → Pair to Mac** (menu wording may vary by Visual Studio release).

Windows can be used for Android development. It cannot compile or deploy the iOS app locally without the Mac build host.

---

## Install the MAUI workload

1. Open **Visual Studio Installer**.
2. Find Visual Studio 2026 and select **Modify**.
3. Open the **Workloads** tab.
4. Select **.NET Multi-platform App UI development**.
5. Keep the recommended Android SDK and OpenJDK components selected.
6. Apply the installation changes.
7. Open Developer PowerShell or PowerShell and verify the SDK/workloads:

   ```powershell
   dotnet --list-sdks
   dotnet workload list
   ```

8. Confirm that the MAUI project template is available:

   ```powershell
   dotnet new list maui
   ```

If Visual Studio reports missing workload components, run the following from the directory containing the app project:

```powershell
dotnet workload restore .\EMOTIX360.App\EMOTIX360.App.csproj
```

---

## Open, build, and run

1. Open `EMOTIX360.App.sln` in Visual Studio 2026.
2. Allow NuGet restore to complete.
3. Set `EMOTIX360.App` as the startup project.
4. Select an Android emulator or connect an Android phone with USB debugging enabled.
5. Select the Android target and run the project.

You can also build from a terminal in the directory containing the solution:

```powershell
dotnet restore .\EMOTIX360.App.sln
dotnet build .\EMOTIX360.App\EMOTIX360.App.csproj -f net10.0-android
```

To run on a connected Android device or emulator, use Visual Studio's Run/Debug controls after selecting the target device.

For iOS, pair Visual Studio with the Mac first, then select an iOS simulator or connected iOS device as the deployment target.

---

## Project layout

```text
EMOTIX360_MAUI/
├── EMOTIX360.App.sln
├── EMOTIX360.App/
│   ├── EMOTIX360.App.csproj
│   ├── App.xaml
│   ├── App.xaml.cs
│   ├── AppShell.xaml
│   ├── AppShell.xaml.cs
│   ├── MainPage.xaml
│   ├── MainPage.xaml.cs
│   ├── MauiProgram.cs
│   ├── Platforms/
│   │   ├── Android/
│   │   │   ├── AndroidManifest.xml
│   │   │   ├── MainActivity.cs
│   │   │   └── MainApplication.cs
│   │   └── iOS/
│   │       ├── Info.plist
│   │       ├── AppDelegate.cs
│   │       └── Program.cs
│   └── Resources/
│       ├── AppIcon/
│       └── Splash/
└── README.md
```

As the BLE feature set grows, keep transport logic separate from page/UI code. A suggested future structure is:

```text
EMOTIX360.App/
├── Models/             # Device status and command models
├── Services/
│   ├── IBleService.cs  # Platform-independent BLE contract
│   └── BleService.cs   # BLE scan/connect/GATT implementation
├── ViewModels/         # Dashboard and connection state
├── Views/              # Dashboard, device and settings pages
└── Platforms/          # Android/iOS-specific permissions if needed
```

This is a proposed organisation for the next implementation stage; these folders/services may not yet exist in the starter.

---

## BLE protocol for EMOTIX360

The ESP32-S3 firmware currently advertises with the following device name and GATT identifiers:

| Item | Value |
|---|---|
| BLE device name | `EMOTIX360` |
| Service UUID | `4fafc201-1fb5-459e-8fcc-c5c9c331914b` |
| Command characteristic UUID | `beb5483e-36e1-4688-b7f5-ea07361b26a8` |
| Status characteristic UUID | `e3223119-9445-4e96-a4a1-85358c4046a2` |

Characteristic properties:

- Command: read and write.
- Status: read and notify.

The app's BLE service will need to:

1. Request the platform's required Bluetooth permissions.
2. Scan for the advertised `EMOTIX360` device (preferably matching the service UUID as well).
3. Connect to the discovered peripheral.
4. Discover the service and both characteristics by UUID.
5. Subscribe to status notifications.
6. Encode and write validated command strings to the command characteristic.
7. Handle disconnects, cancellation, timeouts, and reconnection without blocking the UI thread.

### Status message format

The firmware status characteristic is designed for a text payload like:

```text
P=12.3,R=-4.5,WIFI=1,TIME=1,MODE=HEART,BRIGHT=5
```

The app should parse fields defensively. Treat unknown fields or malformed values as unavailable rather than crashing or displaying incorrect state.

### Proposed command messages

The planned command strings are:

```text
MODE=IDLE
MODE=SAND_CLOCK
MODE=WATER
MODE=SAND_GRAVITY
MODE=HEART
MODE=RAIN
MODE=WAVES

BRIGHT=0
BRIGHT=5
BRIGHT=10
BRIGHT=15
```

These strings are a proposed app/firmware contract. The current firmware BLE callback stores the latest received text command in a pending buffer, but a command dispatcher must still consume it and call the relevant AnimationManager/MAX7219 APIs. Do not consider mode or brightness control functional until that firmware handler and the MAUI BLE client have both been implemented and tested.

---

## BLE platform permissions

The starter currently includes the following Android permissions in `Platforms/Android/AndroidManifest.xml`:

- `BLUETOOTH` and `BLUETOOTH_ADMIN` for older Android versions (limited to API 30).
- `BLUETOOTH_SCAN` for BLE discovery on Android 12 and later.
- `BLUETOOTH_CONNECT` for connecting and communicating with the device.
- `ACCESS_FINE_LOCATION` for older Android versions where BLE scanning may require it.

The app must still request the relevant runtime permissions on Android before scanning or connecting. A manifest declaration alone does not grant runtime permission.

The iOS `Info.plist` currently contains `NSBluetoothAlwaysUsageDescription`, explaining why the app uses Bluetooth. Additional iOS permission or lifecycle handling may be required when implementing and testing the BLE service.

---

## BLE library

The initial starter does not yet include a BLE NuGet package or a BLE client implementation. Before adding one, confirm that the selected library supports the project's .NET MAUI target frameworks and both Android and iOS.

Keep the app's BLE API behind an interface so that UI/ViewModel code is not directly tied to a specific BLE package. This makes it easier to update or replace the transport library later.

---

## UI and device-control roadmap

1. Add a BLE service abstraction and the selected cross-platform BLE library.
2. Implement Android permissions and BLE scanning.
3. Discover and connect to the `EMOTIX360` GATT service.
4. Add disconnect handling and reconnect controls.
5. Subscribe to the status characteristic and parse sensor/network/mode values.
6. Connect animation selection controls to `MODE=` writes.
7. Connect the brightness slider to validated `BRIGHT=` writes.
8. Add connection/error/loading states and prevent duplicate commands while a write is pending.
9. Validate on a physical Android phone and ESP32-S3 hardware.
10. Pair with a Mac and validate iOS scanning, connection, notifications, and background/foreground behaviour.

---

## Troubleshooting

| Problem | Checks |
|---|---|
| MAUI template is missing | Verify the .NET MAUI workload is selected in Visual Studio Installer. |
| Workload/SDK error | Check `dotnet --list-sdks` and `dotnet workload list`; run `dotnet workload restore`. |
| Android build cannot find SDK | Open Visual Studio's Android SDK settings and confirm the SDK/platform tools are installed. |
| App installs but cannot scan | Check runtime Bluetooth permissions, Bluetooth state, and Android version. |
| Device is not discovered | Confirm the ESP32-S3 is powered, BLE advertising is active, and its advertised name is `EMOTIX360`. |
| GATT service is not found | Confirm the ESP32 firmware advertises the expected service UUID and the client discovers services after connecting. |
| iOS build fails on Windows | Pair Visual Studio to a compatible Mac with Xcode installed; iOS builds do not run locally on Windows alone. |

---

## Related documentation

- [Repository overview](../README.md)
- [ESP32-S3 firmware and hardware guide](../EMOTIX360%20-%20S3/README.md)
- [Microsoft .NET MAUI documentation](https://learn.microsoft.com/dotnet/maui/)
- [Visual Studio downloads](https://visualstudio.microsoft.com/vs/)

---

## Repository

[github.com/Dmxddrx/EMOTIX360](https://github.com/Dmxddrx/EMOTIX360)
