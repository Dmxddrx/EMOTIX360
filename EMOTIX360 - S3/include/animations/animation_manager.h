
#pragma once

#include <Arduino.h>

namespace AnimationManager {

// Available matrix animation modes.
enum class Mode : uint8_t {
    IDLE,
    SAND_CLOCK,    // Original randomized hourglass animation
    WATER,         // Physics-based water simulation
    SAND_GRAVITY   // Physics-based sand particle simulation
};

// Initialize the AnimationManager.
// Must be called after MAX7219_Init().
bool begin(Mode initialMode = Mode::SAND_CLOCK);

// Create the FreeRTOS animation task.
bool startTask();

// Request an animation mode change.
// The matrix is changed by the AnimationManager task itself.
bool setMode(Mode mode);

// Check whether the animation task has been created.
bool isTaskRunning();

} // namespace AnimationManager
