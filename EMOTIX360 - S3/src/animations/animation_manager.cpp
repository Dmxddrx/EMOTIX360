
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

// ------------------------------------------------------------
// PUBLISH DEVICE STATUS TO MOBILE APP
// ------------------------------------------------------------

/*
 * Publish a read-only snapshot of EMOTIX360.
 *
 * The AnimationManager owns the active mode, so it is the
 * correct place to obtain the current animation name.
 *
 * Brightness is READ ONLY here.
 * The current MAX7219 intensity is reported, but never changed.
 */
void publishDeviceStatus(Mode activeMode)
{
    MPU6050::Data imu{};

    // Do not publish incomplete sensor data.
    if (!MPU6050::getData(imu) || !imu.valid) {
        return;
    }

    BluetoothManager::setStatus(
        imu.pitchDeg,
        imu.rollDeg,

        WifiManager::isConnected(),

        WifiManager::isTimeValid(),

        getModeName(activeMode),

        // Read current hardware intensity only.
        MAX7219_GetBrightness()
    );
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


    // Update the active mode.
    activeMode = newMode;

    Serial.printf(
        "[ANIMATION] Active mode: %s (%u)\n",
        getModeName(newMode),
        (unsigned int)newMode
    );
}


// ------------------------------------------------------------
// ANIMATION FREERTOS TASK
// ------------------------------------------------------------

void animationTask(void *parameter)
{
    (void)parameter;

    Mode activeMode = Mode::IDLE;
    Mode requestedMode = Mode::IDLE;
    uint32_t lastStatusPublishMs = 0;

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
        // Check whether another module requested a mode change.
        // The queue holds only the latest requested mode.
        if (xQueueReceive(modeQueue, &requestedMode, 0) == pdTRUE) {
            applyMode(requestedMode, activeMode);
        }

        const uint32_t nowMs = millis();

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