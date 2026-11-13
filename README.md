# EMOTIX360

**A motion-responsive embedded display with a BLE companion app.**

EMOTIX360 combines an ESP32-S3-based interactive display with a mobile application for controlling and monitoring the device. The hardware uses two MAX7219 8×8 LED matrices arranged as an hourglass-shaped 128-LED display, an MPU6050 motion sensor, and a tactile mode-selection button. The firmware provides motion-aware particle simulations and visual effects. The companion application is being developed with .NET MAUI for Android and iOS.

This repository contains both sides of the system:

- **ESP32-S3 firmware** — hardware drivers, sensor acquisition, animation logic, Wi-Fi/time services, and BLE communication.
- **EMOTIX360 mobile app** — a dark-themed .NET MAUI application intended to connect to the device over Bluetooth Low Energy (BLE).

> **Project status:** Development and hardware/software integration are in progress. The firmware includes the main animation architecture and a BLE GATT server. The MAUI project currently provides the initial dashboard and platform setup; BLE scanning, connection, commands, and live notifications are the next app-development stage. Do not treat proposed mobile commands as implemented until the firmware command dispatcher and app BLE client are completed and tested together.

---

## Repository structure

The repository is organised into separate firmware and mobile-app projects:

```text
EMOTIX360/
├── EMOTIX360 - S3/          # ESP32-S3 PlatformIO firmware
│   ├── platformio.ini
│   ├── README.md            # Firmware and hardware guide
│   ├── include/
│   ├── src/
│   └── ...
│
├── EMOTIX360_MAUI/          # Android/iOS companion application
│   ├── EMOTIX360.App.sln
│   ├── EMOTIX360.App/
│   └── README.md            # Mobile app development guide
│
├── .gitignore
└── README.md                # This repository overview
```

If the firmware directory has a different name in your local checkout (for example, `S3`), update the directory name in this diagram to match. Keep each project's README in its own project directory.

### Project-specific documentation

- [ESP32-S3 firmware and hardware guide](<EMOTIX360 - S3/README.md>)
- [EMOTIX360 MAUI app development guide](EMOTIX360_MAUI/README.md)

---

## System overview

```text
┌─────────────────────────────────────┐
│          EMOTIX360 mobile app        │
│           .NET MAUI                  │
│                                     │
│  Device connection / animation UI   │
│  Brightness / device status         │
└──────────────────┬──────────────────┘
                   │ Bluetooth Low Energy (BLE)
                   │ GATT service + characteristics
┌──────────────────▼──────────────────┐
│            ESP32-S3                  │
│                                     │
│  BluetoothManager   WiFiManager     │
│  AnimationManager   MPU6050 task    │
│  ButtonManager      MAX7219 driver  │
└───────────┬─────────────────┬───────┘
            │                 │
       MPU6050 sensor     2 × MAX7219
                         8×8 matrices
```

The phone communicates with the ESP32-S3 directly over BLE. A Wi-Fi connection is not required for local Bluetooth control. Wi-Fi is a separate firmware service used for network connectivity and time synchronization when configured.

---

## Main features

### ESP32-S3 firmware

- ESP32-S3 DevKitC-1 as the main controller.
- PlatformIO project using the Arduino framework.
- Configuration for 16 MB flash and 8 MB OPI PSRAM.
- Two daisy-chained MAX7219 8×8 matrix modules, providing 128 LEDs.
- Logical 16×8 framebuffer for the combined display.
- MPU6050 accelerometer and gyroscope acquisition through a dedicated sensor task.
- Thread-safe motion-data snapshots for animation modules.
- One debounced tactile button for selecting display modes.
- Central AnimationManager task responsible for matrix updates.
- Wi-Fi connection management and NTP time synchronization when credentials are configured.
- BLE GATT server advertising as `EMOTIX360`.

### Display modes

| Mode | Description |
|---|---|
| `IDLE` | Clears the matrix. |
| `SAND_CLOCK` | Original hourglass grain-transfer animation, including upside-down orientation handling. |
| `WATER` | Particle-based liquid simulation using the physical LED-neighbour geometry and motion input. |
| `SAND_GRAVITY` | Granular-particle simulation responding to gravity and movement. |
| `HEART` | Two 8×8 heart icons with a pulsing brightness effect. |
| `RAIN` | Gravity-oriented rain-drop animation. |
| `WAVES` | Travelling wave effect based on physical display coordinates and gravity direction. |

The button-controlled sequence is:

`IDLE → SAND_CLOCK → WATER → SAND_GRAVITY → HEART → RAIN → WAVES → IDLE`

The startup animation is a separate boot effect and is not part of the button sequence. Once a selectable mode is active, it remains active until another button press.

### Mobile application

The MAUI app is intended to provide:

- BLE device discovery and connection.
- Animation-mode selection.
- Matrix brightness control.
- Live device status, including pitch, roll, Wi-Fi connection, time validity, current mode, and brightness.
- Android and iOS targets from one shared .NET MAUI project.

**Current app status:** the supplied starter contains the initial dark dashboard UI and platform permission declarations. It does not yet perform BLE scans, connect to the ESP32-S3, write commands, or subscribe to notifications.

---

## Hardware overview

| Component | Purpose |
|---|---|
| ESP32-S3 DevKitC-1 | Main controller |
| 2 × MAX7219 8×8 modules | 128-LED hourglass display |
| MPU6050 | Accelerometer and gyroscope |
| Tactile push button | Animation-mode selection |
| Logic-level buffer | 3.3 V ESP32-S3 signals to 5 V MAX7219 logic |
| Regulated 5 V supply | MAX7219 power |

### Current ESP32-S3 pin assignment

| Device | Signal | ESP32-S3 GPIO |
|---|---|---:|
| MPU6050 | SDA | GPIO6 |
| MPU6050 | SCL | GPIO7 |
| MPU6050 | INT | GPIO4 |
| MAX7219 chain | CLK | GPIO12 |
| MAX7219 chain | DIN / MOSI | GPIO11 |
| MAX7219 chain | CS / LOAD | GPIO10 |
| Animation button | Input | GPIO14 |

Check the firmware's `pin_config.h` before wiring. The firmware configuration is the authority if these assignments change.

### Power and electrical notes

- Power the MAX7219 modules from a stable, regulated 5 V supply sized for the selected brightness and display load.
- Use a suitable 5 V logic buffer for MAX7219 DIN, CLK, and CS/LOAD signals.
- Do not apply 5 V logic signals directly to ESP32-S3 GPIO pins.
- Connect grounds between the ESP32-S3, sensor, level shifter, matrix modules, and external matrix supply.
- Avoid back-feeding the development board or USB supply from an external 5 V matrix supply.
- Check the MPU6050 module's voltage requirements before connecting it to 3.3 V.

For the full assembly notes, see the [ESP32-S3 firmware and hardware guide](<EMOTIX360 - S3/README.md>).

---

## BLE interface

The firmware BLE manager uses the following identifiers. The mobile application should use these same values to discover and communicate with the device.

| BLE item | Value |
|---|---|
| Advertised device name | `EMOTIX360` |
| GATT service UUID | `4fafc201-1fb5-459e-8fcc-c5c9c331914b` |
| Command characteristic UUID | `beb5483e-36e1-4688-b7f5-ea07361b26a8` |
| Status characteristic UUID | `e3223119-9445-4e96-a4a1-85358c4046a2` |
| Command properties | Read, Write |
| Status properties | Read, Notify |

The status characteristic is designed to publish a text payload in this format:

```text
P=12.3,R=-4.5,WIFI=1,TIME=1,MODE=HEART,BRIGHT=5
```

Fields:

- `P`: pitch in degrees.
- `R`: roll in degrees.
- `WIFI`: Wi-Fi connected (`1`) or disconnected (`0`).
- `TIME`: time valid (`1`) or not valid (`0`).
- `MODE`: active animation name.
- `BRIGHT`: MAX7219 brightness value.

The firmware's BLE manager accepts text writes and places the latest received command in a pending-command buffer for the application to retrieve. A command dispatcher still needs to connect that buffer to the relevant firmware controls before commands can operate the animations or brightness.

### Proposed command format

The following strings are the planned app-to-device command format. They are **proposed protocol values**, not confirmation that end-to-end command execution is already implemented:

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

Keep command parsing strict: validate the complete command and allowed value before changing the active mode or brightness. Do not let arbitrary BLE text directly control hardware.

---

## Development environment

### ESP32-S3 firmware

- Visual Studio Code.
- PlatformIO IDE extension.
- Espressif ESP32 platform for PlatformIO.
- ESP32-S3 DevKitC-1 and a USB data cable.

The current firmware configuration targets:

- Board: `esp32-s3-devkitc-1`.
- CPU: 240 MHz.
- Flash: 16 MB.
- PSRAM: 8 MB OPI.
- Partition table: `default_16MB.csv`.
- Serial monitor: 115200 baud.

Open the firmware directory containing `platformio.ini` in VS Code. Build and upload using PlatformIO:

```bash
pio run
pio run --target upload
pio device monitor --baud 115200
```

If Wi-Fi is enabled, provide local credentials through the firmware's private `arduino_secrets.h` configuration. Do not commit real SSIDs, passwords, certificates, or other credentials. The repository `.gitignore` is intended to keep local secrets and build outputs out of Git.

### Android/iOS companion app

The mobile application is located in `EMOTIX360_MAUI/` and uses:

- Visual Studio 2026.
- .NET MAUI workload.
- .NET 10 target frameworks: `net10.0-android` and `net10.0-ios`.
- Android SDK and the Android toolchain installed by the MAUI workload.

Open `EMOTIX360.App.sln` from the MAUI directory. See [EMOTIX360_MAUI/README.md](EMOTIX360_MAUI/README.md) for app-specific setup and build instructions.

Android can be built and tested from Windows. iOS compilation and deployment require a compatible Mac build host with Xcode, paired to Visual Studio. The iOS target is included in the project, but it has not been validated on a physical iOS device yet.

---

## Repository setup

Clone the repository:

```bash
git clone https://github.com/Dmxddrx/EMOTIX360.git
cd EMOTIX360
```

Then open the relevant subproject:

- Firmware: open `EMOTIX360 - S3/` in VS Code with PlatformIO.
- Mobile app: open `EMOTIX360_MAUI/EMOTIX360.App.sln` in Visual Studio 2026.

Build the firmware and mobile app independently. The two projects have separate toolchains and dependency sets.

---

## Current development roadmap

1. Complete physical calibration and validation of the ESP32-S3 matrix geometry and motion response.
2. Verify the firmware BLE server with a BLE client such as nRF Connect.
3. Implement the firmware command dispatcher for validated `MODE=` and `BRIGHT=` commands.
4. Implement BLE scanning, connection, service discovery, command writes, and status notifications in the MAUI app.
5. Connect the dashboard controls and live status cards to actual BLE data.
6. Test reconnect behaviour, Android permissions, and longer-running BLE sessions.
7. Pair with a Mac and validate the iOS target when iOS testing is available.

---

## Limitations and validation notes

- Water and SandGravity are discrete particle simulations, not full computational fluid-dynamics models.
- Physical LED-neighbour geometry depends on the actual matrix placement and may need calibration for the final enclosure.
- MPU6050 tilt estimates can be affected by vibration and linear acceleration. The MPU6050 has no magnetometer and does not provide absolute compass heading.
- The original SandClock and SandGravity are separate animations; changes to one should not unintentionally alter the other.
- Wi-Fi, NTP, BLE, and the mobile app are supporting subsystems. Their presence in the repository does not mean every end-to-end feature has been fully validated.
- Verify the completed hardware, electrical levels, sensor axes, BLE behaviour, and mobile app on real devices before treating the system as production-ready.

---

## Repository

[github.com/Dmxddrx/EMOTIX360](https://github.com/Dmxddrx/EMOTIX360)
