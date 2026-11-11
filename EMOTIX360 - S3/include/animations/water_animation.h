
#pragma once

#include <Arduino.h>

namespace WaterAnimation {

// Initialize the water simulation.
// Call after MAX7219_Init() and MPU6050 initialization.
void begin();

// Reset the water distribution to its initial state.
void reset(uint32_t nowMs);

// Update the water simulation.
// Called periodically by AnimationManager.
void update(uint32_t nowMs);

} // namespace WaterAnimation
