
#pragma once

#include <Arduino.h>

namespace SmallAnimations {

// Small animation effects.
enum class Effect : uint8_t {
    STARTUP,
    HEART,
    RAIN,
    WAVES
};

// Initialize physical geometry and start the startup animation.
bool begin();

// Restart the small-animation sequence.
void reset(uint32_t nowMs);

// Update the current animation.
// Called from AnimationManager.
void update(uint32_t nowMs);

// Select a specific effect.
bool setEffect(Effect effect);

// Manually advance to the next effect.
bool nextEffect();

// Get the current effect.
Effect getEffect();

// Stop the small-animation sequence.
void end();

// Check initialization status.
bool isReady();

} // namespace SmallAnimations
