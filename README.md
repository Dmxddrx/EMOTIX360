# EMOTIX360

**A motion-responsive interactive display powered by ESP32-S3.**

EMOTIX360 is an embedded display project that combines a pair of MAX7219 LED matrices with an MPU6050 motion sensor. Instead of acting as a conventional clock, the device uses its hourglass-shaped 128-LED arrangement to display motion-aware particle simulations and selectable visual effects.

The firmware is organised into separate drivers, sensor handling, animation modules, and a central animation manager. A single button selects the active display mode, while the selected animation continues running until the next button press.

## Project status

**ESP32-S3 migration and hardware integration are in progress.** The firmware includes the main animation architecture, MAX7219 driver, MPU6050 data handling, and button-controlled mode selection. Physical calibration and validation should be completed on the assembled device before treating all motion and particle behaviour as final.

This README describes the current ESP32-S3 design. It is not the wiring or software guide for the earlier ESP32-C3 Water Matrix Clock prototype.

## Features

- ESP32-S3 DevKitC-1 as the main controller, configured for 16 MB flash and 8 MB OPI PSRAM.
- Two daisy-chained MAX7219 8×8 LED matrices, forming a 128-LED hourglass-shaped display.
- A logical 16×8 framebuffer mapped to the two physically rotated matrix modules.
- MPU6050 accelerometer and gyroscope input for motion-aware animation behaviour.
- A dedicated sensor task that collects motion data at approximately 100 Hz and provides safe data snapshots to the animation modules.
- **SandClock:** the original hourglass animation, including orientation detection and display transformation when the device is turned upside down.
- **Water:** a particle-based liquid simulation that responds to device orientation.
- **SandGravity:** a granular-particle simulation that responds to gravity and device movement.
- **Heart:** two 8×8 heart icons with a brightness pulse.
- **Rain:** a continuously running rain animation.
- **Waves:** a continuously running wave animation.
- One debounced tactile button to cycle through the animation modes.
- A central AnimationManager task that owns matrix updates, avoiding concurrent animation tasks writing to the same display.
- PlatformIO project configuration with a custom 16 MB flash partition table.

## Hardware

### Main components

| Component | Purpose |
|---|---|
| ESP32-S3 DevKitC-1 | Main controller |
| 2 × MAX7219 8×8 matrix modules | 128-LED display |
| MPU6050 module | Accelerometer and gyroscope |
| Tactile push button | Selects the next animation mode |
| Regulated 5 V supply | Powers the MAX7219 modules |
| Logic-level buffer | Interfaces ESP32-S3 3.3 V signals with the 5 V MAX7219 logic |

An OLED is not required for the matrix animation system and is not part of the currently active hardware setup.

### ESP32-S3 pin assignment

The current project pin mapping is:

| Device | Signal | ESP32-S3 GPIO |
|---|---|---:|
| MPU6050 | SDA | GPIO6 |
| MPU6050 | SCL | GPIO7 |
| MPU6050 | INT | GPIO4 |
| MAX7219 chain | CLK | GPIO12 |
| MAX7219 chain | DIN | GPIO11 (MOSI) |
| MAX7219 chain | CS / LOAD | GPIO10 |
| Animation button | Button input | GPIO14 |

Check `pin_config.h` before wiring. The source configuration is the authority if the pin assignment is changed in a later revision.

### Power and logic-level considerations

- Power the MPU6050 from 3.3 V when using a module that supports 3.3 V operation.
- Power the MAX7219 modules from a stable, regulated 5 V supply.
- Connect the grounds of the ESP32-S3, MPU6050, level shifter, MAX7219 modules, and external matrix supply together.
- Use a suitable 5 V logic buffer for MAX7219 DIN, CLK, and CS. A 74AHCT125 or 74HCT125 is a suitable choice for translating the ESP32-S3's 3.3 V logic signals to 5 V logic.
- Do not allow 5 V logic signals to reach ESP32-S3 GPIO pins.
- Do not connect an external 5 V matrix supply in a way that back-feeds the development board or USB supply. Keep the power arrangement appropriate for the board and USB connection.
- Size the 5 V supply for the actual matrix current at the selected brightness. The MAX7219 modules can draw considerably more current when many LEDs are illuminated.

## Matrix arrangement

The display uses two 8×8 MAX7219 modules mounted above and below one another and rotated in opposite directions to create an hourglass-like shape.

- The upper physical module is the slave module.
- The lower physical module is the master module.
- The firmware represents the complete display using a logical 16×8 framebuffer.
- Logical X coordinates `0–7` address the upper/slave module.
- Logical X coordinates `8–15` address the lower/master module.
- The physical module rotations are handled by the display mapping and geometry logic; the logical framebuffer remains consistent.

The `WaterGeometry` module calculates the physical LED positions and builds a neighbour graph. Water and SandGravity use this graph to move particles between physically adjacent LEDs, including connections between the two modules. The geometry depends on the actual placement and spacing of the assembled modules, so it may need calibration for the final enclosure.

## Animation modes and controls

The animation button is connected to **GPIO14** and uses an active-LOW input with the internal pull-up enabled. A debouncing interval is used to prevent one physical press from being registered multiple times.

The button cycles through the following modes:

| Press | Mode | Behaviour |
|---:|---|---|
| Startup | `IDLE` | Matrix is blank |
| 1 | `SAND_CLOCK` | Original hourglass animation |
| 2 | `WATER` | Motion-responsive liquid particles |
| 3 | `SAND_GRAVITY` | Motion-responsive granular particles |
| 4 | `HEART` | Two pulsing heart icons |
| 5 | `RAIN` | Continuous rain animation |
| 6 | `WAVES` | Continuous wave animation |
| 7 | `IDLE` | Clears the matrix |
| 8 | `SAND_CLOCK` | Sequence repeats |

**The selected mode stays active until another button press.** HEART, RAIN, and WAVES are independent modes; they do not automatically switch to one another after a timeout. Their internal animation continues while the mode is selected. SandClock may perform its own hourglass cycle without changing the active AnimationManager mode.

The `SMALL_ANIMATIONS` module is an implementation module for the HEART, RAIN, and WAVES effects. It is not a separate button mode.

## Motion sensing

The MPU6050 communicates with the ESP32-S3 over I²C. The driver supports the common MPU6050 I²C addresses `0x68` and `0x69`.

Motion data is collected by the sensor task and made available to the animation modules through a thread-safe snapshot API. This prevents the animation code from reading partially updated sensor values.

The current reference mounting orientation is:

- MPU6050 X+ points toward the front/display face.
- MPU6050 Y+ points to the left side of the device.
- MPU6050 Z+ points downward when the device is in its normal upright position.

The Water and SandGravity modules use the accelerometer and gyroscope data to estimate the gravity direction and particle movement. Verify the axis signs against the actual sensor mounting before tuning the simulation parameters.

The MPU6050 has no magnetometer. The firmware therefore does not obtain an absolute compass heading, and accelerometer-based tilt estimates can be affected by sustained linear acceleration and vibration.

## Firmware architecture

The firmware separates hardware access from animation behaviour.

### Main responsibilities

| Module | Responsibility |
|---|---|
| `main.cpp` | Application entry point |
| `general.cpp / general.h` | Initialises and coordinates the main subsystems |
| `pin_config.h` | Central hardware pin assignments |
| `max7219.cpp / max7219.h` | MAX7219 chain, framebuffer, pixels, and brightness |
| `mpu6050.cpp / mpu6050.h` | MPU6050 communication and motion-data handling |
| `button.cpp / button.h` | Button input, debouncing, and mode cycling |
| `animation_manager.cpp / animation_manager.h` | Mode queue, animation task, and active-mode dispatch |
| `sand_clock.cpp / sand_clock.h` | Original hourglass animation and orientation handling |
| `water_geometry.cpp / water_geometry.h` | Physical LED coordinates and neighbour graph |
| `water_animation.cpp / water_animation.h` | Water particle simulation |
| `sand_gravity.cpp / sand_gravity.h` | Granular particle simulation |
| `small_animations.cpp / small_animations.h` | HEART, RAIN, and WAVES visual effects |

### Animation task model

The AnimationManager owns the MAX7219 display update path. The button manager sends mode requests through a FreeRTOS queue. The animation task processes the latest requested mode and updates only the currently selected animation.

The sensor task independently updates the MPU6050 data snapshot. Animation modules read that snapshot when required; they do not start their own competing matrix-update tasks.

This design keeps mode selection, sensor acquisition, and matrix rendering separate and makes it easier to add new animation modes.

## Software requirements

- Visual Studio Code
- PlatformIO IDE extension
- Espressif ESP32 platform for PlatformIO
- ESP32-S3 DevKitC-1
- USB data cable

The project uses the Arduino framework under PlatformIO. The board, flash, PSRAM, and partition settings are defined by the project's `platformio.ini` and associated partition CSV file.

The current firmware configuration targets:

- Board: `esp32-s3-devkitc-1`
- Flash: 16 MB
- PSRAM: 8 MB OPI
- Partition table: `default_16MB.csv`
- Serial monitor: 115200 baud

Use the configuration committed with the project rather than copying settings from the older ESP32-C3 project.

## Build and upload

1. Install Visual Studio Code and the PlatformIO IDE extension.
2. Clone the repository:

   ```bash
   git clone https://github.com/Dmxddrx/EMOTIX360.git
   ```

3. Open the ESP32-S3 firmware folder (currently `EMOTIX360 - S3`) in Visual Studio Code. Select the directory that contains `platformio.ini`.
4. Connect the ESP32-S3 DevKitC-1 using a USB data cable.
5. Build the firmware using **PlatformIO: Build** or:

   ```bash
   pio run
   ```

6. Upload using **PlatformIO: Upload** or:

   ```bash
   pio run --target upload
   ```

7. Open the serial monitor at 115200 baud:

   ```bash
   pio device monitor --baud 115200
   ```

If the repository contains more than one PlatformIO environment, select the ESP32-S3 environment configured for EMOTIX360 before building or uploading.

## First hardware test

1. Confirm the ESP32-S3 pin assignments against `pin_config.h`.
2. Check that all grounds are connected and that the MAX7219 modules have a stable external 5 V supply.
3. Verify the level shifter connections for DIN, CLK, and CS/LOAD.
4. Power the ESP32-S3 and confirm that the startup diagnostics appear in the serial monitor.
5. Confirm that the MPU6050 is detected at `0x68` or `0x69` and that motion data updates.
6. Test the MAX7219 chain with a simple display pattern. Confirm module order, orientation, and pixel mapping.
7. Press the GPIO14 button repeatedly and verify the sequence: IDLE → SAND_CLOCK → WATER → SAND_GRAVITY → HEART → RAIN → WAVES → IDLE.
8. Tilt and rotate the device to test the Water and SandGravity response. Check that the particle count remains stable and that particles do not become duplicated or lost.
9. Turn the device upside down and verify the original SandClock orientation behaviour.
10. Leave HEART, RAIN, and WAVES selected for an extended period. Confirm that each effect continues running and does not automatically change to another mode.

## Troubleshooting

| Symptom | Checks |
|---|---|
| Matrix remains blank | Check external 5 V power, common ground, level shifter enable pins, DIN/CLK/CS wiring, and MAX7219 chain order. |
| All LEDs remain on or the pattern is corrupted | Check the CLK, DIN, and CS connections and confirm the matrix driver pin mapping. |
| Only one matrix module works | Check the DOUT-to-DIN connection between modules and verify the master/slave order expected by the driver. |
| MPU6050 is not detected | Check SDA on GPIO6, SCL on GPIO7, 3.3 V power, common ground, pull-ups, and addresses `0x68`/`0x69`. |
| Particles move in the wrong direction | Verify the MPU6050 mounting orientation and gravity-axis signs before changing simulation coefficients. |
| Water or SandGravity movement looks physically incorrect | Recheck the matrix module placement and calibrate the physical LED geometry and neighbour-distance assumptions. |
| One button press skips modes | Check the GPIO14 wiring, active-LOW connection, pull-up, and debounce settings. |
| Heart, Rain, or Waves changes to another effect by itself | Check that the automatic effect-duration transitions have been removed from the corresponding SmallAnimations update functions. |

## Notes and limitations

- The Water and SandGravity effects are discrete particle simulations designed for the 128-LED physical layout. They are not full computational fluid-dynamics simulations.
- The physical neighbour graph is based on the configured module geometry. Changes to the mechanical mounting can require geometry recalibration.
- MPU6050 accelerometer and gyroscope readings require correct axis mapping and may be affected by vibration or linear acceleration.
- The MPU6050 does not provide absolute yaw or compass heading.
- The original SandClock and the newer SandGravity mode are separate implementations. Changes to SandGravity should not unintentionally modify the original SandClock behaviour.
- OLED, Wi-Fi/NTP, and BLE functionality are not required for the core matrix animation system. Treat them as optional supporting features only when they are enabled and configured in the current firmware branch.
- Validate power, signal levels, sensor readings, matrix mapping, and animation behaviour on the completed hardware before considering the device production-ready.

## Repository

[https://github.com/Dmxddrx/EMOTIX360](https://github.com/Dmxddrx/EMOTIX360)