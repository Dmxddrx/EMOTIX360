
#include "button.h"

#include "pin_config.h"
#include "animation_manager.h"

#include <Arduino.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace ButtonManager {
namespace {

// ------------------------------------------------------------
// BUTTON TASK CONFIGURATION
// ------------------------------------------------------------

constexpr uint32_t BUTTON_TASK_STACK_SIZE = 3072;
constexpr UBaseType_t BUTTON_TASK_PRIORITY = 1;

// Button sampling interval.
constexpr uint32_t BUTTON_POLL_INTERVAL_MS = 10;

// Required stable time before accepting a state change.
constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;

// ------------------------------------------------------------
// INTERNAL STATE
// ------------------------------------------------------------

TaskHandle_t buttonTaskHandle = nullptr;

bool initialized = false;

// Current animation tracked by the button manager.
// This must match AnimationManager's initial mode.
AnimationManager::Mode currentMode =
    AnimationManager::Mode::IDLE;

// ------------------------------------------------------------
// ANIMATION MODE SWITCHING
// ------------------------------------------------------------

/*
 * TOGGLE ANIMATION MODE
 *
 * Animation sequence:
 *
 *     SAND_CLOCK
 *          |
 *          v
 *       WATER
 *          |
 *          v
 *     SAND_GRAVITY
 *          |
 *          v
 *     SAND_CLOCK
 *
 * The original SandClock and both physics simulations
 * are controlled by AnimationManager.
 */
void toggleAnimation()
{
    AnimationManager::Mode nextMode;

    switch (currentMode)
    {
        case AnimationManager::Mode::SAND_CLOCK:
        {
            nextMode = AnimationManager::Mode::WATER;
            break;
        }

        case AnimationManager::Mode::WATER:
        {
            nextMode = AnimationManager::Mode::SAND_GRAVITY;
            break;
        }

        case AnimationManager::Mode::SAND_GRAVITY:
        {
            nextMode = AnimationManager::Mode::SAND_CLOCK;
            break;
        }

        case AnimationManager::Mode::IDLE:
        default:
        {
            nextMode = AnimationManager::Mode::SAND_CLOCK;
            break;
        }
    }

    if (!AnimationManager::setMode(nextMode))
    {
        Serial.println("[BUTTON] Animation mode request failed");
        return;
    }

    currentMode = nextMode;

    switch (currentMode)
    {
        case AnimationManager::Mode::SAND_CLOCK:
            Serial.println("[BUTTON] Original SandClock selected");
            break;

        case AnimationManager::Mode::WATER:
            Serial.println("[BUTTON] WaterAnimation selected");
            break;

        case AnimationManager::Mode::SAND_GRAVITY:
            Serial.println("[BUTTON] SandGravity selected");
            break;

        default:
            break;
    }
}

// ------------------------------------------------------------
// BUTTON MONITORING TASK
// ------------------------------------------------------------

/*
 * BUTTON TASK
 *
 * Active LOW button:
 *
 *     HIGH = Released.
 *     LOW  = Pressed.
 *
 * Software debouncing:
 *
 * 1. Read the physical button state.
 * 2. Detect a raw state change.
 * 3. Wait until the state remains unchanged for
 *    BUTTON_DEBOUNCE_MS.
 * 4. Accept the new stable state.
 * 5. Toggle animation only on a stable press.
 *
 * Holding the button does not repeatedly change animations.
 */
void buttonTask(void *parameter)
{
    (void)parameter;

    // Read the initial state to prevent an unwanted
    // animation change if the button is held during startup.
    bool lastRawPressed =
        (digitalRead(PinConfig::BUTTON_ANIMATION) == LOW);

    bool stablePressed = lastRawPressed;

    uint32_t lastRawChangeMs = millis();

    TickType_t lastWakeTime = xTaskGetTickCount();

    TickType_t pollInterval =
        pdMS_TO_TICKS(BUTTON_POLL_INTERVAL_MS);

    // Ensure a valid FreeRTOS interval.
    if (pollInterval == 0) {
        pollInterval = 1;
    }

    while (true)
    {
        const bool rawPressed =
            (digitalRead(PinConfig::BUTTON_ANIMATION) == LOW);

        const uint32_t nowMs = millis();

        // Detect a raw electrical state change.
        if (rawPressed != lastRawPressed)
        {
            lastRawPressed = rawPressed;
            lastRawChangeMs = nowMs;
        }

        // Accept the new state only after it remains stable.
        if ((uint32_t)(nowMs - lastRawChangeMs) >=
            BUTTON_DEBOUNCE_MS)
        {
            if (stablePressed != lastRawPressed)
            {
                stablePressed = lastRawPressed;

                // Only a stable LOW transition counts as a press.
                if (stablePressed)
                {
                    toggleAnimation();
                }
            }
        }

        // Schedule the next button sample without busy polling.
        vTaskDelayUntil(&lastWakeTime, pollInterval);
    }
}

} // namespace

// ------------------------------------------------------------
// PUBLIC API
// ------------------------------------------------------------

bool begin(AnimationManager::Mode initialMode)
{
    if (initialized) {
        return true;
    }

    // Configure the tactile button with the internal pull-up.
    pinMode(PinConfig::BUTTON_ANIMATION, INPUT_PULLUP);

    // Synchronize the button manager with AnimationManager.
    currentMode = initialMode;

    initialized = true;

    Serial.printf(
        "[BUTTON] Animation button initialized on GPIO%u\n",
        PinConfig::BUTTON_ANIMATION
    );

    return true;
}

bool startTask()
{
    if (!initialized) {
        Serial.println("[BUTTON] Initialize button before starting task");
        return false;
    }

    if (buttonTaskHandle != nullptr) {
        return true;
    }

    const BaseType_t result = xTaskCreate(
        buttonTask,
        "ButtonTask",
        BUTTON_TASK_STACK_SIZE,
        nullptr,
        BUTTON_TASK_PRIORITY,
        &buttonTaskHandle
    );

    if (result != pdPASS)
    {
        buttonTaskHandle = nullptr;

        Serial.println("[BUTTON] Failed to create button task");

        return false;
    }

    Serial.println("[BUTTON] Button task started");

    return true;
}

bool isTaskRunning()
{
    return buttonTaskHandle != nullptr;
}

} // namespace ButtonManager
