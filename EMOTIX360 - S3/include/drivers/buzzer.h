
#ifndef BUZZER_H
#define BUZZER_H

#include <Arduino.h>

namespace Buzzer {

// Available sound events.
enum class Sound : uint8_t {

    STARTUP,

    WIFI_CONNECTING,
    WIFI_CONNECTED,
    WIFI_DISCONNECTED,

    BLE_CONNECTED,
    BLE_DISCONNECTED,

    COMMAND_RECEIVED,

    ERROR
};


// Initialize buzzer hardware and create the sound queue.
bool begin();

// Start the independent FreeRTOS buzzer task.
bool startTask();

// Queue a sound event for playback.
// This function does not wait for the sound to finish.
bool play(Sound sound);

// Stop the current sound and discard queued sounds.
void stop();

// Enable or disable all buzzer sounds.
void setEnabled(bool enabled);

bool isEnabled();

// Check whether the buzzer task is running.
bool isTaskRunning();

}

#endif
