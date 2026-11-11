
#pragma once

#include <Arduino.h>
#include "animation_manager.h"

namespace ButtonManager {

// Initialize the animation button.
// initialMode must match AnimationManager's initial mode.
bool begin(AnimationManager::Mode initialMode);

// Create the button monitoring FreeRTOS task.
bool startTask();

// Check whether the button task has been created.
bool isTaskRunning();

} // namespace ButtonManager
