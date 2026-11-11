
#include "animation_manager.h"

#include "max7219.h"
#include "sand_clock.h"
#include "water_animation.h"
#include "sand_gravity.h"

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
// MODE TRANSITION
// ------------------------------------------------------------

void applyMode(Mode requestedMode, Mode &activeMode)
{
    // Do nothing if the requested mode is already active.
    if (requestedMode == activeMode) {
        return;
    }

    // Clear the previous animation before changing modes.
    if (activeMode == Mode::SAND_CLOCK ||
        activeMode == Mode::WATER ||
        activeMode == Mode::SAND_GRAVITY)
    {
        MAX7219_Clear();
        MAX7219_Update();
    }

    activeMode = requestedMode;

    switch (activeMode)
    {
        case Mode::SAND_CLOCK:
        {
            // Start a fresh SandClock animation cycle.
            SandClock::begin();

            Serial.println("[ANIMATION] SandClock activated");
            break;
        }

        case Mode::WATER:
        {
            WaterAnimation::begin();

            Serial.println("[ANIMATION] WaterAnimation activated");
            break;
        }

        case Mode::SAND_GRAVITY:
        {
            SandGravity::begin();

            Serial.println("[ANIMATION] SandGravity activated");
            break;
        }

        case Mode::IDLE:
        default:
        {
            Serial.println("[ANIMATION] Matrix animations disabled");
            break;
        }
    }
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
        switch (activeMode)
        {
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

            case Mode::IDLE:
            default:
            {
                // Nothing to animate.
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
