
#include "buzzer.h"

#include <Arduino.h>
#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>


namespace Buzzer {
namespace {

// ============================================================
// HARDWARE CONFIGURATION
// ============================================================

// Currently unused by the other EMOTIX360 peripherals.
constexpr uint8_t BUZZER_PIN = 13;

// LEDC configuration.
constexpr uint32_t BUZZER_BASE_FREQUENCY = 2000;
constexpr uint8_t BUZZER_RESOLUTION_BITS = 10;


// ============================================================
// FREERTOS CONFIGURATION
// ============================================================

constexpr uint32_t BUZZER_TASK_STACK_SIZE = 3072;

constexpr UBaseType_t BUZZER_TASK_PRIORITY = 1;

constexpr uint32_t BUZZER_TASK_INTERVAL_MS = 5;

constexpr uint8_t SOUND_QUEUE_LENGTH = 8;


// ============================================================
// SOUND DEFINITIONS
// ============================================================

struct ToneNote {

    uint16_t frequencyHz;

    uint16_t durationMs;

    uint16_t pauseMs;
};


struct SoundSequence {

    const ToneNote *notes;

    uint8_t count;
};

// ------------------------------------------------------------
// STARTUP SOUND
// ------------------------------------------------------------

// Bright ascending C-major arpeggio.
// Plays like a small electronic welcome melody.

constexpr ToneNote STARTUP_NOTES[] = {

    { 523,  75, 35 },   // C5
    { 659,  75, 35 },   // E5
    { 784,  90, 35 },   // G5
    {1047, 110, 45 },   // C6
    {1319, 220,  0 }    // E6 - final sustained note
};


// ------------------------------------------------------------
// WIFI CONNECTING
// ------------------------------------------------------------

// Gentle ascending G-major pattern.
// Indicates that the device is searching for a network.

constexpr ToneNote WIFI_CONNECTING_NOTES[] = {

    { 392,  75, 45 },   // G4
    { 494,  75, 45 },   // B4
    { 587, 130,  0 }   // D5
};


// ------------------------------------------------------------
// WIFI CONNECTED
// ------------------------------------------------------------

// Bright three-note G-major success chime.
// Higher pitch than the connecting sound.

constexpr ToneNote WIFI_CONNECTED_NOTES[] = {

    { 784,  85, 35 },   // G5
    { 988,  95, 35 },   // B5
    {1175, 210,  0 }   // D6
};


// ------------------------------------------------------------
// WIFI DISCONNECTED
// ------------------------------------------------------------

// Descending G-major pattern.
// Sounds like a gentle connection-lost warning.

constexpr ToneNote WIFI_DISCONNECTED_NOTES[] = {

    { 587, 120, 45 },   // D5
    { 494, 130, 45 },   // B4
    { 392, 230,  0 }   // G4
};


// ------------------------------------------------------------
// BLE CONNECTED
// ------------------------------------------------------------

// Smooth ascending E-minor pattern.
// Different musical identity from the Wi-Fi chime.

constexpr ToneNote BLE_CONNECTED_NOTES[] = {

    { 659,  80, 35 },   // E5
    { 784,  90, 35 },   // G5
    { 988, 190,  0 }   // B5
};


// ------------------------------------------------------------
// BLE DISCONNECTED
// ------------------------------------------------------------

// Descending E-minor pattern.

constexpr ToneNote BLE_DISCONNECTED_NOTES[] = {

    { 988, 110, 40 },   // B5
    { 784, 120, 40 },   // G5
    { 659, 220,  0 }   // E5
};


// ------------------------------------------------------------
// COMMAND RECEIVED
// ------------------------------------------------------------

// Two crisp, high-pitched acknowledgement chirps.
// Short enough for frequent command notifications.

constexpr ToneNote COMMAND_RECEIVED_NOTES[] = {

    {1319,  45, 30 },   // E6
    {1568,  70,  0 }   // G6
};


// ------------------------------------------------------------
// ERROR
// ------------------------------------------------------------

// Descending low-pitched warning.
// Clearly different from the pleasant connection sounds.

constexpr ToneNote ERROR_NOTES[] = {

    { 392, 160, 55 },   // G4
    { 311, 180, 55 },   // Eb4
    { 262, 280,  0 }   // C4
};


// ============================================================
// INTERNAL STATE
// ============================================================

QueueHandle_t soundQueue = nullptr;

TaskHandle_t buzzerTaskHandle = nullptr;

bool initialized = false;

std::atomic<bool> enabled{true};

std::atomic<bool> stopRequested{false};


// ============================================================
// SOUND SEQUENCE LOOKUP
// ============================================================

SoundSequence getSoundSequence(Sound sound)
{
    switch (sound)
    {
        case Sound::STARTUP:

            return {
                STARTUP_NOTES,
                sizeof(STARTUP_NOTES) / sizeof(STARTUP_NOTES[0])
            };


        case Sound::WIFI_CONNECTING:

            return {
                WIFI_CONNECTING_NOTES,
                sizeof(WIFI_CONNECTING_NOTES) /
                    sizeof(WIFI_CONNECTING_NOTES[0])
            };


        case Sound::WIFI_CONNECTED:

            return {
                WIFI_CONNECTED_NOTES,
                sizeof(WIFI_CONNECTED_NOTES) /
                    sizeof(WIFI_CONNECTED_NOTES[0])
            };


        case Sound::WIFI_DISCONNECTED:

            return {
                WIFI_DISCONNECTED_NOTES,
                sizeof(WIFI_DISCONNECTED_NOTES) /
                    sizeof(WIFI_DISCONNECTED_NOTES[0])
            };


        case Sound::BLE_CONNECTED:

            return {
                BLE_CONNECTED_NOTES,
                sizeof(BLE_CONNECTED_NOTES) /
                    sizeof(BLE_CONNECTED_NOTES[0])
            };


        case Sound::BLE_DISCONNECTED:

            return {
                BLE_DISCONNECTED_NOTES,
                sizeof(BLE_DISCONNECTED_NOTES) /
                    sizeof(BLE_DISCONNECTED_NOTES[0])
            };


        case Sound::COMMAND_RECEIVED:

            return {
                COMMAND_RECEIVED_NOTES,
                sizeof(COMMAND_RECEIVED_NOTES) /
                    sizeof(COMMAND_RECEIVED_NOTES[0])
            };


        case Sound::ERROR:

            return {
                ERROR_NOTES,
                sizeof(ERROR_NOTES) /
                    sizeof(ERROR_NOTES[0])
            };


        default:

            return { nullptr, 0 };
    }
}


// ============================================================
// PWM CONTROL
// ============================================================

/*
 * START TONE
 *
 * LEDC generates a 50% duty-cycle square wave.
 *
 * The frequency determines the audible pitch.
 */
bool startTone(uint16_t frequencyHz)
{
    if (frequencyHz == 0) {
        return false;
    }

    const uint32_t configuredFrequency =
        ledcWriteTone(
            BUZZER_PIN,
            frequencyHz
        );

    return configuredFrequency != 0;
}


/*
 * STOP TONE
 *
 * Setting the LEDC frequency to zero disables its output.
 */
void stopTone()
{
    ledcWriteTone(
        BUZZER_PIN,
        0
    );
}


// ============================================================
// PLAYBACK STATE
// ============================================================

struct PlaybackState {

    SoundSequence sequence = { nullptr, 0 };

    uint8_t noteIndex = 0;

    bool active = false;

    bool notePlaying = false;

    uint32_t phaseStartedMs = 0;
};


// ------------------------------------------------------------
// START A NOTE
// ------------------------------------------------------------

bool startCurrentNote(
    PlaybackState &state,
    uint32_t nowMs
)
{
    if (!state.active ||
        state.sequence.notes == nullptr ||
        state.noteIndex >= state.sequence.count)
    {
        state.active = false;
        stopTone();
        return false;
    }

    const ToneNote &note =
        state.sequence.notes[state.noteIndex];

    if (!startTone(note.frequencyHz))
    {
        Serial.println(
            "[BUZZER] Failed to generate tone"
        );

        state.active = false;
        state.notePlaying = false;

        stopTone();

        return false;
    }

    state.notePlaying = true;

    state.phaseStartedMs = nowMs;

    return true;
}


// ------------------------------------------------------------
// START A SOUND SEQUENCE
// ------------------------------------------------------------

void startSequence(
    PlaybackState &state,
    Sound sound,
    uint32_t nowMs
)
{
    stopTone();

    state.sequence = getSoundSequence(sound);

    state.noteIndex = 0;

    state.active =
        state.sequence.notes != nullptr &&
        state.sequence.count > 0;

    state.notePlaying = false;

    state.phaseStartedMs = nowMs;

    if (state.active) {
        startCurrentNote(state, nowMs);
    }
}


// ------------------------------------------------------------
// ADVANCE TO NEXT NOTE
// ------------------------------------------------------------

void advanceToNextNote(
    PlaybackState &state,
    uint32_t nowMs
)
{
    ++state.noteIndex;

    if (state.noteIndex >= state.sequence.count)
    {
        state.active = false;
        state.notePlaying = false;

        stopTone();

        return;
    }

    startCurrentNote(state, nowMs);
}


// ============================================================
// UPDATE PLAYBACK STATE
// ============================================================

/*
 * UPDATE PLAYBACK
 *
 * This function never uses delay().
 *
 * Each note and pause is controlled using millis().
 *
 * This keeps the buzzer task responsive to:
 *
 *     - New sound requests.
 *     - Stop requests.
 *     - Buzzer disable requests.
 */
void updatePlayback(
    PlaybackState &state,
    uint32_t nowMs
)
{
    if (!state.active) {
        return;
    }

    if (state.noteIndex >= state.sequence.count)
    {
        state.active = false;
        stopTone();
        return;
    }

    const ToneNote &note =
        state.sequence.notes[state.noteIndex];


    // --------------------------------------------------------
    // TONE PLAYING
    // --------------------------------------------------------

    if (state.notePlaying)
    {
        if ((uint32_t)(nowMs - state.phaseStartedMs) >=
            note.durationMs)
        {
            stopTone();

            state.notePlaying = false;

            state.phaseStartedMs = nowMs;

            // Advance immediately when there is no pause.
            if (note.pauseMs == 0) {
                advanceToNextNote(state, nowMs);
            }
        }

        return;
    }


    // --------------------------------------------------------
    // PAUSE BETWEEN NOTES
    // --------------------------------------------------------

    if ((uint32_t)(nowMs - state.phaseStartedMs) >=
        note.pauseMs)
    {
        advanceToNextNote(state, nowMs);
    }
}


// ============================================================
// BUZZER FREERTOS TASK
// ============================================================

/*
 * BUZZER TASK
 *
 * This task is the only owner of the LEDC buzzer output.
 *
 * Other application tasks only submit sound events through
 * the FreeRTOS queue.
 *
 * A new sound request replaces the sound currently playing.
 */
void buzzerTask(void *parameter)
{
    (void)parameter;

    Serial.println(
        "[BUZZER] Task started"
    );

    PlaybackState playback{};

    TickType_t lastWakeTime =
        xTaskGetTickCount();

    const TickType_t updatePeriod =
        pdMS_TO_TICKS(BUZZER_TASK_INTERVAL_MS);


    for (;;)
    {
        // ----------------------------------------------------
        // RECEIVE NEW SOUND REQUEST
        // ----------------------------------------------------

        Sound requestedSound;

        if (xQueueReceive(
                soundQueue,
                &requestedSound,
                0) == pdTRUE)
        {
            // Cancel any previous sound sequence.
            startSequence(
                playback,
                requestedSound,
                millis()
            );
        }


        // ----------------------------------------------------
        // HANDLE STOP REQUEST
        // ----------------------------------------------------

        if (stopRequested.exchange(
                false,
                std::memory_order_relaxed))
        {
            playback.active = false;

            playback.notePlaying = false;

            stopTone();
        }


        // ----------------------------------------------------
        // HANDLE DISABLED STATE
        // ----------------------------------------------------

        if (!enabled.load(std::memory_order_relaxed))
        {
            playback.active = false;

            playback.notePlaying = false;

            stopTone();
        }
        else
        {
            // Update the current note / pause state.
            updatePlayback(
                playback,
                millis()
            );
        }


        // ----------------------------------------------------
        // WAIT FOR NEXT UPDATE
        // ----------------------------------------------------

        vTaskDelayUntil(
            &lastWakeTime,
            updatePeriod
        );
    }
}

} // namespace


// ============================================================
// PUBLIC API
// ============================================================


// ------------------------------------------------------------
// INITIALIZE BUZZER
// ------------------------------------------------------------

bool begin()
{
    if (initialized) {
        return true;
    }

    Serial.println(
        "[BUZZER] Initializing passive buzzer"
    );


    // --------------------------------------------------------
    // CREATE SOUND EVENT QUEUE
    // --------------------------------------------------------

    soundQueue = xQueueCreate(
        SOUND_QUEUE_LENGTH,
        sizeof(Sound)
    );

    if (soundQueue == nullptr)
    {
        Serial.println(
            "[BUZZER] Failed to create sound queue"
        );

        return false;
    }


    // --------------------------------------------------------
    // ATTACH LEDC PWM TO BUZZER PIN
    // --------------------------------------------------------

    if (!ledcAttach(
            BUZZER_PIN,
            BUZZER_BASE_FREQUENCY,
            BUZZER_RESOLUTION_BITS))
    {
        Serial.println(
            "[BUZZER] LEDC initialization failed"
        );

        vQueueDelete(soundQueue);

        soundQueue = nullptr;

        return false;
    }

    // Start with the buzzer silent.
    stopTone();

    enabled.store(
        true,
        std::memory_order_relaxed
    );

    initialized = true;

    Serial.printf(
        "[BUZZER] Ready on GPIO%u\n",
        BUZZER_PIN
    );

    return true;
}


// ------------------------------------------------------------
// START BUZZER TASK
// ------------------------------------------------------------

bool startTask()
{
    if (!initialized || soundQueue == nullptr) {

        Serial.println(
            "[BUZZER] Manager not initialized"
        );

        return false;
    }

    if (buzzerTaskHandle != nullptr) {
        return true;
    }

    const BaseType_t result = xTaskCreate(
        buzzerTask,
        "BuzzerTask",
        BUZZER_TASK_STACK_SIZE,
        nullptr,
        BUZZER_TASK_PRIORITY,
        &buzzerTaskHandle
    );

    if (result != pdPASS)
    {
        buzzerTaskHandle = nullptr;

        Serial.println(
            "[BUZZER] Failed to create task"
        );

        return false;
    }

    Serial.println(
        "[BUZZER] Task created"
    );

    return true;
}


// ------------------------------------------------------------
// PLAY SOUND
// ------------------------------------------------------------

/*
 * Add a sound request to the queue.
 *
 * This function returns immediately.
 *
 * If the queue is full, the new request is rejected.
 */
bool play(Sound sound)
{
    if (!initialized ||
        soundQueue == nullptr ||
        buzzerTaskHandle == nullptr ||
        !enabled.load(std::memory_order_relaxed))
    {
        return false;
    }

    // Reject invalid sound values.
    if (getSoundSequence(sound).count == 0) {
        return false;
    }

    // A new request is allowed after a previous stop request.
    stopRequested.store(
        false,
        std::memory_order_relaxed
    );

    return xQueueSend(
        soundQueue,
        &sound,
        0
    ) == pdPASS;
}


// ------------------------------------------------------------
// STOP PLAYBACK
// ------------------------------------------------------------

void stop()
{
    if (soundQueue != nullptr) {
        xQueueReset(soundQueue);
    }

    stopRequested.store(
        true,
        std::memory_order_relaxed
    );
}


// ------------------------------------------------------------
// ENABLE / DISABLE
// ------------------------------------------------------------

void setEnabled(bool state)
{
    enabled.store(
        state,
        std::memory_order_relaxed
    );

    if (!state) {
        stop();
    }
}


bool isEnabled()
{
    return enabled.load(
        std::memory_order_relaxed
    );
}


// ------------------------------------------------------------
// TASK STATUS
// ------------------------------------------------------------

bool isTaskRunning()
{
    return buzzerTaskHandle != nullptr;
}

} // namespace Buzzer
