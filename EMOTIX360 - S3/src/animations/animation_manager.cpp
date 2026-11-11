
#include "animation_manager.h"

#include "max7219.h"
#include "sand_clock.h"
#include "water_animation.h"
#include "sand_gravity.h"
#include "small_animations.h"

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

// ------------------------------------------------------------
// INTERNAL STATE
// ------------------------------------------------------------

QueueHandle_t modeQueue = nullptr;
TaskHandle_t animationTaskHandle = nullptr;

bool initialized = false;

// ------------------------------------------------------------
// SMALL ANIMATION MODE HELPERS
// ------------------------------------------------------------

// Check whether a mode belongs to the SmallAnimations module.
bool isSmallAnimationMode(Mode mode)
{
    switch (mode) {
        case Mode::SMALL_ANIMATIONS:
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
    switch (mode) {
        case Mode::HEART:
            return SmallAnimations::Effect::HEART;

        case Mode::RAIN:
            return SmallAnimations::Effect::RAIN;

        case Mode::WAVES:
            return SmallAnimations::Effect::WAVES;

        case Mode::SMALL_ANIMATIONS:
        default:
            return SmallAnimations::Effect::STARTUP;
    }
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
 * SmallAnimations contains four selectable effects:
 *
 *     STARTUP
 *     HEART
 *     RAIN
 *     WAVES
 *
 * These effects share the same SmallAnimations module.
 */
void applyMode(Mode newMode, Mode &activeMode)
{
    // Ignore requests for the currently active mode.
    if (newMode == activeMode) {
        return;
    }

    const bool wasSmallAnimation =
        isSmallAnimationMode(activeMode);

    const bool isNewSmallAnimation =
        isSmallAnimationMode(newMode);


    // --------------------------------------------------------
    // EXIT CURRENT MODULE
    // --------------------------------------------------------

    // Restore brightness when leaving SmallAnimations entirely.
    //
    // Do not call end() when switching between HEART, RAIN,
    // WAVES, and STARTUP because they share the same module.
    if (wasSmallAnimation && !isNewSmallAnimation) {
        SmallAnimations::end();
    }


    // --------------------------------------------------------
    // DISPLAY CLEAR
    // --------------------------------------------------------

    // Restore your existing MAX7219 matrix-clear operation here.
    //
    // I have not assumed a clear-function name because the
    // max7219.h API was not included in the supplied code.
    //
    // Make sure the display is cleared when entering IDLE.


    // --------------------------------------------------------
    // START NEW MODULE
    // --------------------------------------------------------

    switch (newMode) {

        case Mode::IDLE:
            // No animation. The matrix should remain blank.
            break;


        case Mode::SAND_CLOCK:
            SandClock::begin();
            break;


        case Mode::WATER:
            WaterAnimation::begin();
            break;


        case Mode::SAND_GRAVITY:
            SandGravity::begin();
            break;


        case Mode::SMALL_ANIMATIONS:
        case Mode::HEART:
        case Mode::RAIN:
        case Mode::WAVES:

            // Initialise SmallAnimations only when entering
            // from a different animation module.
            if (!wasSmallAnimation) {
                SmallAnimations::begin();
            }

            // Select the requested individual effect.
            SmallAnimations::setEffect(
                getSmallAnimationEffect(newMode)
            );

            break;
    }

    // Record the newly active mode.
    activeMode = newMode;
}

// ------------------------------------------------------------
// ANIMATION FREERTOS TASK
// ------------------------------------------------------------

void animationTask(void *parameter)
{
    (void)parameter;

    Mode activeMode = Mode::IDLE;
    Mode requestedMode = Mode::IDLE;

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

        // Only the AnimationManager task updates the matrix.
        switch (activeMode) {

            case Mode::IDLE:
                // Nothing to update.
                break;


            case Mode::SAND_CLOCK:
                SandClock::update(nowMs);
                break;


            case Mode::WATER:
                WaterAnimation::update(nowMs);
                break;


            case Mode::SAND_GRAVITY:
                SandGravity::update(nowMs);
                break;


            case Mode::SMALL_ANIMATIONS:
            case Mode::HEART:
            case Mode::RAIN:
            case Mode::WAVES:
                // All four effects are updated by the same module.
                SmallAnimations::update(nowMs);
                break;
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
    if (xQueueOverwrite(modeQueue, &initialMode) != pdPASS) {
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

    if (result != pdPASS) {
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
