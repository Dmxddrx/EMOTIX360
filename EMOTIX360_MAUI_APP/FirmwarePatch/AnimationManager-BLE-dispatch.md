# ESP32-S3 firmware patch: consume app commands and publish live status

Apply this patch in the `EMOTIX360 - S3` PlatformIO project. It is intentionally a targeted patch, not a replacement for your complete animation manager.

The current firmware already creates the BLE GATT characteristics and exposes `BluetoothManager::takeCommand()` / `BluetoothManager::setStatus()`. The missing part is connecting those APIs to the AnimationManager task.

## A. Update `include/animations/small_animations.h`

Add this declaration inside `namespace SmallAnimations`, after `Effect getEffect();`:

```cpp
// Change the user-selected base brightness while retaining the
// HEART pulse and STARTUP breathing effects.
void setBaseBrightness(uint8_t brightness);
```

## B. Update `src/animations/small_animations.cpp`

Add this public function near the other public API functions, for example immediately after `Effect getEffect()`:

```cpp
// ------------------------------------------------------------
// SET USER BASE BRIGHTNESS
// ------------------------------------------------------------

void setBaseBrightness(uint8_t brightness)
{
    if (brightness > 15) {
        return;
    }

    // HEART and STARTUP use this value as their animation base.
    heartBaseBrightness = brightness;
    startupBaseBrightness = brightness;

    // When the SmallAnimations module is stopped (for example while
    // WATER or SAND_GRAVITY is active), remember the base and update
    // the matrix directly. The next SmallAnimations::begin() will
    // capture this brightness as its starting value.
    if (!initialized)
    {
        MAX7219_SetBrightness(brightness);
        return;
    }

    if (currentEffect == Effect::STARTUP)
    {
        // Keep the active startup animation's captured base in sync.
        startupBrightnessCaptured = true;
        MAX7219_SetBrightness(brightness);
    }
    else if (currentEffect == Effect::HEART)
    {
        // Start the next heartbeat from its normal minimum phase.
        const uint8_t startBrightness =
            (uint8_t)lroundf(brightness * HEART_PULSE_MIN_FACTOR);

        MAX7219_SetBrightness(startBrightness);
    }
    else
    {
        // RAIN/WAVES do not animate brightness, so apply it directly.
        MAX7219_SetBrightness(brightness);
    }
}
```

This is important: simply calling `MAX7219_SetBrightness()` from the BLE handler is not sufficient while HEART/STARTUP is running. Their animation code writes brightness repeatedly. This setter updates the base values used by those effects.

## C. Update `src/animations/animation_manager.cpp`

### C1. Add these includes

At the top with the other includes:

```cpp
#include "bluetooth_manager.h"
#include "mpu6050.h"
#include "wifi_manager.h"

#include <stdlib.h>
#include <string.h>
```

The PlatformIO project already adds the `include/services`, `include/drivers`, and `include/animations` directories to its include path.

### C2. Add a stored user brightness value

Inside the anonymous namespace, alongside `initialized`, add:

```cpp
// User-selected brightness (0..15). This is kept separate from
// the instantaneous intensity used by HEART/STARTUP pulse effects.
uint8_t configuredBrightness = 5;
```

### C3. Add the command parser helpers

Insert the following code after `applyMode()` and before `animationTask()`:

```cpp
// ------------------------------------------------------------
// BLE COMMAND DISPATCH
// ------------------------------------------------------------

bool parseModeCommand(const char *value, Mode &mode)
{
    if (value == nullptr) {
        return false;
    }

    if (strcmp(value, "IDLE") == 0) {
        mode = Mode::IDLE;
    }
    else if (strcmp(value, "SAND_CLOCK") == 0) {
        mode = Mode::SAND_CLOCK;
    }
    else if (strcmp(value, "WATER") == 0) {
        mode = Mode::WATER;
    }
    else if (strcmp(value, "SAND_GRAVITY") == 0) {
        mode = Mode::SAND_GRAVITY;
    }
    else if (strcmp(value, "HEART") == 0) {
        mode = Mode::HEART;
    }
    else if (strcmp(value, "RAIN") == 0) {
        mode = Mode::RAIN;
    }
    else if (strcmp(value, "WAVES") == 0) {
        mode = Mode::WAVES;
    }
    else {
        return false;
    }

    return true;
}

bool parseBrightnessCommand(const char *value, uint8_t &brightness)
{
    if (value == nullptr || *value == '\0') {
        return false;
    }

    char *end = nullptr;
    const long parsed = strtol(value, &end, 10);

    // Reject partial numbers, signs with no number, and extra text.
    if (end == value || *end != '\0' || parsed < 0 || parsed > 15) {
        return false;
    }

    brightness = (uint8_t)parsed;
    return true;
}

void processBluetoothCommand(Mode &activeMode)
{
    char command[48] = {};

    if (!BluetoothManager::takeCommand(command, sizeof(command))) {
        return;
    }

    // MODE=... -------------------------------------------------
    if (strncmp(command, "MODE=", 5) == 0)
    {
        Mode requestedMode;

        if (!parseModeCommand(command + 5, requestedMode))
        {
            Serial.printf("[BLE] Rejected mode command: %s\n", command);
            return;
        }

        Serial.printf("[BLE] Applying mode command: %s\n", command);
        applyMode(requestedMode, activeMode);
        return;
    }

    // BRIGHT=... -----------------------------------------------
    if (strncmp(command, "BRIGHT=", 7) == 0)
    {
        uint8_t requestedBrightness = 0;

        if (!parseBrightnessCommand(command + 7, requestedBrightness))
        {
            Serial.printf("[BLE] Rejected brightness command: %s\n", command);
            return;
        }

        configuredBrightness = requestedBrightness;

        // Update SmallAnimations' stored base even while that module is
        // stopped. Its setter applies the live intensity when appropriate.
        SmallAnimations::setBaseBrightness(requestedBrightness);

        Serial.printf(
            "[BLE] Brightness set to %u/15\n",
            requestedBrightness
        );

        return;
    }

    Serial.printf("[BLE] Rejected unknown command: %s\n", command);
}


// ------------------------------------------------------------
// BLE STATUS PUBLISHER
// ------------------------------------------------------------

void publishBluetoothStatus(Mode activeMode)
{
    MPU6050::Data imu{};

    float pitchDeg = 0.0f;
    float rollDeg = 0.0f;

    if (MPU6050::getData(imu) && imu.valid)
    {
        pitchDeg = imu.pitchDeg;
        rollDeg = imu.rollDeg;
    }

    BluetoothManager::setStatus(
        pitchDeg,
        rollDeg,
        WifiManager::isConnected(),
        WifiManager::isTimeValid(),
        getModeName(activeMode),
        configuredBrightness
    );
}
```

### C4. Consume the command inside `animationTask()`

Inside the `while (true)` loop, directly after the existing mode-queue check:

```cpp
// BLE commands are consumed here so the AnimationManager task
// remains the sole owner of matrix and animation state changes.
processBluetoothCommand(activeMode);
```

The surrounding code should look like:

```cpp
if (xQueueReceive(modeQueue, &requestedMode, 0) == pdTRUE) {
    applyMode(requestedMode, activeMode);
}

processBluetoothCommand(activeMode);

const uint32_t nowMs = millis();
```

### C5. Publish status once per second

Inside the same `while (true)` loop, after the animation `switch` and before `vTaskDelayUntil()`, add:

```cpp
// Publish a compact BLE status notification once per second.
static uint32_t lastBluetoothStatusMs = 0;

if ((uint32_t)(nowMs - lastBluetoothStatusMs) >= 1000)
{
    lastBluetoothStatusMs = nowMs;
    publishBluetoothStatus(activeMode);
}
```

### C6. Initialize the stored brightness

In `AnimationManager::begin(Mode initialMode)`, after confirming MAX7219 has already been initialized and before setting `initialized = true`, add:

```cpp
configuredBrightness = MAX7219_GetBrightness();
```

## D. Expected firmware behaviour after patch

The AnimationManager task now accepts exactly these validated commands:

```text
MODE=IDLE
MODE=SAND_CLOCK
MODE=WATER
MODE=SAND_GRAVITY
MODE=HEART
MODE=RAIN
MODE=WAVES
BRIGHT=0     through BRIGHT=15
```

It rejects `STARTUP`, unknown modes, malformed commands, and brightness values outside 0..15.

The status characteristic is updated and notified once per second with:

```text
P=12.3,R=-4.5,WIFI=1,TIME=1,MODE=HEART,BRIGHT=5
```

`BRIGHT` reports the user-selected base brightness, not the instantaneous LED intensity while HEART/STARTUP is pulsing.

## E. Important review before building

- Confirm your current `animation_manager.cpp` still has `getModeName()`, `applyMode()`, and the same `animationTask()` structure as the repository version inspected for this patch.
- Confirm `SmallAnimations::setBaseBrightness()` is declared/defined in your current branch. The BLE dispatcher calls it even when SmallAnimations is not active, so the user's brightness choice is retained for the next HEART/STARTUP effect.
- Do not call `AnimationManager::setMode()` from the BLE callback. The BLE callback only fills the pending command buffer; the AnimationManager task consumes it.
- This patch does not alter button mode cycling or the existing startup-to-heart transition.
