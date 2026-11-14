
#include "animation_manager.h"

#include "max7219.h"
#include "sand_clock.h"
#include "water_animation.h"
#include "sand_gravity.h"
#include "small_animations.h"

#include "bluetooth_manager.h"
#include "mpu6050.h"
#include "wifi_manager.h"

#include <Arduino.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

namespace AnimationManager {
namespace {

// ------------------------------------------------------------
// TASK CONFIGURATION
// ------------------------------------------------------------

constexpr uint32_t ANIMATION_UPDATE_INTERVAL_MS = 10;

constexpr uint32_t ANIMATION_TASK_STACK_SIZE = 4096;
constexpr UBaseType_t ANIMATION_TASK_PRIORITY = 1;

constexpr uint32_t DEVICE_STATUS_PUBLISH_INTERVAL_MS = 1000;

// ------------------------------------------------------------
// INTERNAL STATE
// ------------------------------------------------------------

QueueHandle_t modeQueue = nullptr;
TaskHandle_t animationTaskHandle = nullptr;

bool initialized = false;

// ------------------------------------------------------------
// MOBILE APP BRIGHTNESS CONFIGURATION
// ------------------------------------------------------------

// User-selected base brightness, range 0..15.
//
// SmallAnimations uses this value as its brightness reference.
// Its heartbeat and startup effects can still pulse normally.

uint8_t configuredBrightness = 5;

// ------------------------------------------------------------
// MODE NAME HELPER
// ------------------------------------------------------------

// Return a readable name for each animation mode.
// This makes Serial diagnostics easier to understand.

const char* getModeName(Mode mode)
{
    switch (mode)
    {
        case Mode::IDLE:
            return "IDLE";

        case Mode::STARTUP:
            return "STARTUP";

        case Mode::SAND_CLOCK:
            return "SAND_CLOCK";

        case Mode::WATER:
            return "WATER";

        case Mode::SAND_GRAVITY:
            return "SAND_GRAVITY";

        case Mode::HEART:
            return "HEART";

        case Mode::RAIN:
            return "RAIN";

        case Mode::WAVES:
            return "WAVES";

        default:
            return "UNKNOWN";
    }
}


// ------------------------------------------------------------
// SMALL ANIMATION MODE HELPERS
// ------------------------------------------------------------

// Check whether a mode belongs to the SmallAnimations module.

bool isSmallAnimationMode(Mode mode)
{
    switch (mode)
    {
        case Mode::STARTUP:
        case Mode::HEART:
        case Mode::RAIN:
        case Mode::WAVES:
            return true;

        default:
            return false;
    }
}


// Convert an AnimationManager mode into its corresponding
// SmallAnimations effect.

SmallAnimations::Effect getSmallAnimationEffect(Mode mode)
{
    switch (mode)
    {
        case Mode::STARTUP:
            return SmallAnimations::Effect::STARTUP;

        case Mode::HEART:
            return SmallAnimations::Effect::HEART;

        case Mode::RAIN:
            return SmallAnimations::Effect::RAIN;

        case Mode::WAVES:
            return SmallAnimations::Effect::WAVES;

        default:
            return SmallAnimations::Effect::HEART;
    }
}

// ============================================================
// DEVICE STATUS PUBLISHER
// ============================================================

/*
 * Publishes the actual device state to the connected app.
 *
 * The active animation mode comes directly from the
 * AnimationManager task.
 *
 * Brightness reports the configured base level, rather
 * than the instantaneous heartbeat pulse intensity.
 *
 * No animation or display state is modified here.
 */

bool publishDeviceStatus(Mode activeMode)
{
    MPU6050::Data imu{};

    if (!MPU6050::getData(imu) || !imu.valid) {
        return false;
    }

    BluetoothManager::setStatus(
        imu.pitchDeg,
        imu.rollDeg,
        WifiManager::isConnected(),
        WifiManager::isTimeValid(),
        getModeName(activeMode),
        configuredBrightness
    );

    return true;
}

// ------------------------------------------------------------
// MODE TRANSITION
// ------------------------------------------------------------

/*
 * APPLY MODE
 *
 * The activeMode parameter is passed by reference because
 * it belongs to the AnimationManager task.
 *
 * SmallAnimations contains four effects:
 *
 *     STARTUP
 *     HEART
 *     RAIN
 *     WAVES
 *
 * SPECIAL CASE:
 *
 *     STARTUP -> HEART
 *
 *     Preserve the final startup transition frame because
 *     it already contains the two completed heart icons.
 *
 *     SmallAnimations handles the brightness handoff.
 */
void applyMode(Mode newMode, Mode &activeMode)
{
    if (newMode == activeMode) {
        return;
    }

    const bool wasSmallAnimation =
        isSmallAnimationMode(activeMode);

    const bool isNewSmallAnimation =
        isSmallAnimationMode(newMode);


    // --------------------------------------------------------
    // CHECK FOR THE SMOOTH STARTUP -> HEART HANDOFF
    // --------------------------------------------------------

    const bool preserveStartupHeartFrame =
        activeMode == Mode::STARTUP &&
        newMode == Mode::HEART;


    // --------------------------------------------------------
    // STOP SMALL ANIMATIONS WHEN LEAVING THEIR MODULE
    // --------------------------------------------------------

    if (wasSmallAnimation && !isNewSmallAnimation) {
        SmallAnimations::end();
    }


    // --------------------------------------------------------
    // CLEAR PREVIOUS FRAME
    // --------------------------------------------------------

    /*
     * Do not clear the matrix during STARTUP -> HEART.
     *
     * The startup transition has already created the exact
     * double-heart framebuffer.
     *
     * Clearing it here would destroy that framebuffer.
     *
     * SmallAnimations::setEffect(HEART) intentionally skips
     * renderHeart() during this special handoff.
     */

    if (!preserveStartupHeartFrame)
    {
        MAX7219_Clear();
        MAX7219_Update();
    }


    // --------------------------------------------------------
    // INITIALIZE THE REQUESTED MODE
    // --------------------------------------------------------

    switch (newMode)
    {
        case Mode::IDLE:
        {
            break;
        }


        case Mode::STARTUP:
        case Mode::HEART:
        case Mode::RAIN:
        case Mode::WAVES:
        {
            bool effectAlreadySelected = false;

            // Initialize SmallAnimations only when entering
            // its module from another animation module.
            if (!wasSmallAnimation)
            {
                if (!SmallAnimations::begin())
                {
                    Serial.println(
                        "[ANIMATION] SmallAnimations initialization failed"
                    );

                    activeMode = Mode::IDLE;
                    return;
                }

                /*
                 * IMPORTANT:
                 *
                 * SmallAnimations::begin() automatically
                 * selects Effect::STARTUP.
                 *
                 * Therefore, when entering STARTUP for the
                 * first time, do not select STARTUP again.
                 *
                 * For HEART, RAIN or WAVES, we still need to
                 * explicitly select the requested effect.
                 */

                if (newMode == Mode::STARTUP) {
                    effectAlreadySelected = true;
                }
            }

            // Select the requested effect when necessary.
            if (!effectAlreadySelected)
            {
                if (!SmallAnimations::setEffect(
                        getSmallAnimationEffect(newMode)))
                {
                    Serial.println(
                        "[ANIMATION] Failed to select SmallAnimations effect"
                    );

                    if (!wasSmallAnimation) {
                        SmallAnimations::end();
                    }

                    activeMode = Mode::IDLE;
                    return;
                }
            }

            break;
        }


        case Mode::SAND_CLOCK:
        {
            SandClock::begin();
            break;
        }


        case Mode::WATER:
        {
            WaterAnimation::begin();
            break;
        }


        case Mode::SAND_GRAVITY:
        {
            SandGravity::begin();
            break;
        }
    }

    // --------------------------------------------------------
    // RESTORE THE CONFIGURED BRIGHTNESS BASE
    // --------------------------------------------------------

    /*
    * Every mode transition must retain the mobile app's
    * configured brightness.
    *
    * SmallAnimations handles its own dynamic brightness.
    * Other modes use the MAX7219 intensity directly.
    */

    if (isSmallAnimationMode(newMode))
    {
        SmallAnimations::setBaseBrightness(
            configuredBrightness
        );
    }
    else
    {
        MAX7219_SetBrightness(
            configuredBrightness
        );
    }

    // Update the active mode.
    activeMode = newMode;

    Serial.printf(
        "[ANIMATION] Active mode: %s (%u)\n",
        getModeName(newMode),
        (unsigned int)newMode
    );
}


// ============================================================
// BLUETOOTH COMMAND DISPATCHER
// ============================================================

// ------------------------------------------------------------
// PARSE ANIMATION MODE
// ------------------------------------------------------------

bool parseBluetoothMode(
    const char *value,
    Mode &mode
)
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

    // STARTUP is intentionally excluded.
    // It remains a one-time boot animation.

    return true;
}


// ------------------------------------------------------------
// APPLY MOBILE APP BRIGHTNESS
// ------------------------------------------------------------

void applyBluetoothBrightness(uint8_t brightness, Mode activeMode)
{
    if (brightness > 15) {
        brightness = 15;
    }

    configuredBrightness = brightness;

    // Update the base brightness used by SmallAnimations.
    //
    // HEART and STARTUP will continue their own brightness
    // animation using the newly selected base value.
    //
    // RAIN and WAVES have no independent brightness pulse,
    // so SmallAnimations applies the selected intensity directly.

    SmallAnimations::setBaseBrightness(brightness);

    // Other animation modules use the MAX7219 intensity directly.
    if (!isSmallAnimationMode(activeMode)) {
        MAX7219_SetBrightness(brightness);
    }

    Serial.printf(
        "[BLE] Brightness override applied: %u/15\n",
        brightness
    );
}


// ------------------------------------------------------------
// PROCESS PENDING BLUETOOTH COMMAND
// ------------------------------------------------------------

/*
 * Called only from AnimationManager::animationTask().
 *
 * Supported commands:
 *
 *     MODE=IDLE
 *     MODE=SAND_CLOCK
 *     MODE=WATER
 *     MODE=SAND_GRAVITY
 *     MODE=HEART
 *     MODE=RAIN
 *     MODE=WAVES
 *
 *     BRIGHT=0 ... BRIGHT=15
 *
 * The existing BluetoothManager callback only receives
 * and stores commands. This function actually executes them.
 *
 * Returns true when a command has been consumed.
 */

bool processBluetoothCommand(Mode &activeMode)
{
    char command[48] = {};

    if (!BluetoothManager::takeCommand(
            command,
            sizeof(command)))
    {
        return false;
    }

    // Remove trailing line endings and whitespace.
    size_t length = strlen(command);

    while (length > 0)
    {
        const char last = command[length - 1];

        if (last == '\r' ||
            last == '\n' ||
            last == ' ' ||
            last == '\t')
        {
            command[--length] = '\0';
        }
        else
        {
            break;
        }
    }

    if (length == 0) {
        return true;
    }


    // --------------------------------------------------------
    // ANIMATION MODE OVERRIDE
    // --------------------------------------------------------

    if (strncmp(command, "MODE=", 5) == 0)
    {
        Mode requestedMode;

        const char *modeValue = command + 5;

        if (!parseBluetoothMode(modeValue, requestedMode))
        {
            Serial.printf(
                "[BLE] Invalid animation mode: %s\n",
                modeValue
            );

            return true;
        }

        // The AnimationManager task owns the matrix,
        // so apply the mode directly from this task.
        applyMode(requestedMode, activeMode);

        if (activeMode == requestedMode)
        {
            Serial.printf(
                "[BLE] Command applied: MODE=%s\n",
                getModeName(activeMode)
            );
        }
        else
        {
            Serial.println(
                "[BLE] Animation mode override failed"
            );
        }

        return true;
    }


    // --------------------------------------------------------
    // BRIGHTNESS OVERRIDE
    // --------------------------------------------------------

    if (strncmp(command, "BRIGHT=", 7) == 0)
    {
        const char *value = command + 7;

        if (*value == '\0')
        {
            Serial.println("[BLE] Invalid brightness command");
            return true;
        }

        // Accept decimal digits only.
        for (const char *p = value; *p != '\0'; ++p)
        {
            if (*p < '0' || *p > '9')
            {
                Serial.println("[BLE] Invalid brightness value");
                return true;
            }
        }

        char *endPointer = nullptr;

        const long brightness = strtol(
            value,
            &endPointer,
            10
        );

        if (endPointer == value ||
            *endPointer != '\0' ||
            brightness < 0 ||
            brightness > 15)
        {
            Serial.println(
                "[BLE] Brightness must be between 0 and 15"
            );

            return true;
        }

        applyBluetoothBrightness(
            static_cast<uint8_t>(brightness),
            activeMode
        );

        return true;
    }


    // --------------------------------------------------------
    // UNKNOWN COMMAND
    // --------------------------------------------------------

    Serial.printf(
        "[BLE] Unsupported command: %s\n",
        command
    );

    return true;
}


// ------------------------------------------------------------
// ANIMATION FREERTOS TASK
// ------------------------------------------------------------

void animationTask(void *parameter)
{
    (void)parameter;

    Mode activeMode = Mode::IDLE;
    Mode requestedMode = Mode::IDLE;

    // BLE STATUS TRACKING
    uint32_t lastStatusPublishMs = 0;
    Mode lastPublishedMode = Mode::IDLE;
    bool statusModeInitialized = false;

    // Apply the initial mode selected during begin().
    if (xQueueReceive(modeQueue, &requestedMode, 0) == pdTRUE) {
        applyMode(requestedMode, activeMode);
    }

    TickType_t lastWakeTime = xTaskGetTickCount();

    TickType_t updatePeriod =
        pdMS_TO_TICKS(ANIMATION_UPDATE_INTERVAL_MS);

    // Guarantee a non-zero FreeRTOS delay period.
    if (updatePeriod == 0) {
        updatePeriod = 1;
    }

    while (true)
    {

        // PROCESS PHYSICAL BUTTON MODE REQUEST
        if (xQueueReceive(modeQueue, &requestedMode, 0) == pdTRUE)
        {
            applyMode(requestedMode, activeMode);
        }


        // PROCESS MOBILE APP BLE COMMAND
        const bool bluetoothCommandProcessed =
            processBluetoothCommand(activeMode);
      
        // PUBLISH DEVICE STATUS
        const uint32_t nowMs = millis();

        const bool modeChanged =
            !statusModeInitialized ||
            activeMode != lastPublishedMode;

        const bool statusIntervalElapsed =
            (uint32_t)(nowMs - lastStatusPublishMs) >=
            DEVICE_STATUS_PUBLISH_INTERVAL_MS;


        // Publish immediately after:
        //     - A physical button mode change.
        //     - A mobile app command.
        //     - The regular one-second interval.

        if (bluetoothCommandProcessed ||
            modeChanged ||
            statusIntervalElapsed)
        {
            const bool statusReady =
                publishDeviceStatus(activeMode);

            lastStatusPublishMs = nowMs;

            lastPublishedMode = activeMode;

            statusModeInitialized = true;

            if (statusReady &&
                BluetoothManager::isConnected() &&
                (bluetoothCommandProcessed || modeChanged))
            {
                Serial.printf(
                    "[BLE] Device status published: MODE=%s, BRIGHT=%u\n",
                    getModeName(activeMode),
                    configuredBrightness
                );
            }

            if (!statusReady && modeChanged)
            {
                Serial.println(
                    "[BLE] Status pending: MPU6050 data unavailable"
                );
            }
        }

        // Publish a live device snapshot every second.
        // This does not modify the display or animation brightness.

        if ((uint32_t)(nowMs - lastStatusPublishMs) >=
            DEVICE_STATUS_PUBLISH_INTERVAL_MS)
        {
            publishDeviceStatus(activeMode);

            lastStatusPublishMs = nowMs;
        }

        // Only the AnimationManager task updates the matrix.
        switch (activeMode)
        {
            case Mode::IDLE:
            {
                // Nothing to update.
                break;
            }


            case Mode::SAND_CLOCK:
            {
                SandClock::update(nowMs);
                break;
            }


            case Mode::WATER:
            {
                WaterAnimation::update(nowMs);
                break;
            }


            case Mode::SAND_GRAVITY:
            {
                SandGravity::update(nowMs);
                break;
            }


            case Mode::STARTUP:
            {
                SmallAnimations::update(nowMs);

                // The startup animation is a one-time boot effect.
                // Return to HEART automatically after its final pulse.
                if (SmallAnimations::isStartupComplete())
                {
                    Serial.println(
                        "[ANIMATION] Boot animation finished. Entering HEART."
                    );

                    applyMode(Mode::HEART, activeMode);
                }

                break;
            }


            case Mode::HEART:
            case Mode::RAIN:
            case Mode::WAVES:
            {
                // All four effects are updated by the same module.
                SmallAnimations::update(nowMs);
                break;
            }
        }

        // Wait until the next scheduled animation update.
        // This avoids busy polling.
        vTaskDelayUntil(&lastWakeTime, updatePeriod);
    }
}

} // namespace


// ------------------------------------------------------------
// PUBLIC API
// ------------------------------------------------------------

bool begin(Mode initialMode)
{
    if (initialized) {
        return true;
    }

    // Capture the MAX7219's initial user brightness.
    configuredBrightness = MAX7219_GetBrightness();

    // A one-element queue stores the latest requested mode.
    modeQueue = xQueueCreate(1, sizeof(Mode));

    if (modeQueue == nullptr) {
        Serial.println("[ANIMATION] Failed to create mode queue");
        return false;
    }

    // Store the initial animation mode.
    if (xQueueOverwrite(modeQueue, &initialMode) != pdPASS)
    {
        vQueueDelete(modeQueue);
        modeQueue = nullptr;

        Serial.println("[ANIMATION] Failed to set initial mode");
        return false;
    }

    initialized = true;

    Serial.println("[ANIMATION] Manager initialized");

    return true;
}


bool startTask()
{
    if (!initialized || modeQueue == nullptr) {
        Serial.println("[ANIMATION] Manager not initialized");
        return false;
    }

    if (animationTaskHandle != nullptr) {
        return true;
    }

    const BaseType_t result = xTaskCreate(
        animationTask,
        "AnimationTask",
        ANIMATION_TASK_STACK_SIZE,
        nullptr,
        ANIMATION_TASK_PRIORITY,
        &animationTaskHandle
    );

    if (result != pdPASS)
    {
        animationTaskHandle = nullptr;

        Serial.println("[ANIMATION] Failed to create task");
        return false;
    }

    Serial.println("[ANIMATION] Task started");

    return true;
}


bool setMode(Mode mode)
{
    if (!initialized || modeQueue == nullptr) {
        return false;
    }

    // Queue length is one, so newer requests replace older ones.
    return xQueueOverwrite(modeQueue, &mode) == pdPASS;
}


bool isTaskRunning()
{
    return animationTaskHandle != nullptr;
}

} // namespace AnimationManager