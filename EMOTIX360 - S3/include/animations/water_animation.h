
#pragma once

#include <Arduino.h>

namespace WaterAnimation {

// Initialize geometry, create the initial liquid distribution
// and prepare the physics engine.
void begin();

// Reset the simulation and place the liquid according
// to the current gravity direction.
void reset(uint32_t nowMs);

// Update MPU6050 gravity estimation and liquid physics.
// Call periodically from AnimationManager.
void update(uint32_t nowMs);

// Check whether the water simulation is initialized.
bool isReady();

// Return the current number of occupied water cells.
// A correctly operating simulation should always return 64.
uint8_t getWaterCount();

} // namespace WaterAnimation
