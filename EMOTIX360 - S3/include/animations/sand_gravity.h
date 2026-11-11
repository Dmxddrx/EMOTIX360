
#pragma once

#include <Arduino.h>

namespace SandGravity {

// Initialize the physical geometry and 64 sand particles.
void begin();

// Reset the sand distribution according to the current
// gravity direction.
void reset(uint32_t nowMs);

// Update gravity, particle movement and rendering.
// Called periodically from AnimationManager.
void update(uint32_t nowMs);

// Check whether the simulation is initialized.
bool isReady();

// Return the number of occupied sand positions.
// A valid simulation always contains 64 grains.
uint8_t getSandCount();

} // namespace SandGravity
