#include "wifi_manager.h"

#include "display_manager.h"
#include "buzzer.h"

#include <WiFi.h>
#include <Wire.h>

#include <stdio.h>
#include <string.h>
#include <atomic>

#include <esp_sntp.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>


#if __has_include("arduino_secrets.h")
#include "arduino_secrets.h"
#else
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#endif


namespace WifiManager {
namespace {

// ------------------------------------------------------------
// WIFI CONFIGURATION
// ------------------------------------------------------------

constexpr char TIME_ZONE[] = "<+0530>-5:30";

constexpr char NTP_SERVER_1[] = "pool.ntp.org";
constexpr char NTP_SERVER_2[] = "time.google.com";
constexpr char NTP_SERVER_3[] = "time.cloudflare.com";

constexpr uint32_t RECONNECT_INTERVAL_MS = 15000;

// ------------------------------------------------------------
// FREERTOS CONFIGURATION
// ------------------------------------------------------------

constexpr uint32_t WIFI_TASK_STACK_SIZE = 4096;

constexpr UBaseType_t WIFI_TASK_PRIORITY = 2;

constexpr BaseType_t WIFI_TASK_CORE = 0;

constexpr uint8_t WIFI_EVENT_QUEUE_LENGTH = 8;


// ------------------------------------------------------------
// EVENT DEFINITIONS
// ------------------------------------------------------------

enum class ManagerEvent : uint8_t {
    GOT_IP,
    DISCONNECTED,
    TIME_SYNCED
};

QueueHandle_t wifiEventQueue = nullptr;

TaskHandle_t wifiTaskHandle = nullptr;


// ------------------------------------------------------------
// DRIVER STATE
// ------------------------------------------------------------

bool initialized = false;

bool ntpConfigured = false;

std::atomic<bool> connected{false};

std::atomic<bool> ntpHasSynced{false};


// ------------------------------------------------------------
// WIFI CREDENTIAL CHECK
// ------------------------------------------------------------

bool hasCredentials()
{
    return WIFI_SSID[0] != '\0';
}


// ------------------------------------------------------------
// EVENT QUEUE HELPER
// ------------------------------------------------------------

void enqueueEvent(ManagerEvent event)
{
    if (wifiEventQueue != nullptr) {

        // Callbacks execute in task context, not ISR context.
        xQueueSend(
            wifiEventQueue,
            &event,
            0
        );
    }
}


// ------------------------------------------------------------
// WIFI EVENT CALLBACK
// ------------------------------------------------------------

void wifiEventHandler(WiFiEvent_t event)
{
    switch (event) {

        case ARDUINO_EVENT_WIFI_STA_GOT_IP:

            enqueueEvent(ManagerEvent::GOT_IP);

            break;


        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:

        case ARDUINO_EVENT_WIFI_STA_LOST_IP:

            enqueueEvent(ManagerEvent::DISCONNECTED);

            break;


        default:
            break;
    }
}


// ------------------------------------------------------------
// NTP TIME SYNCHRONIZATION CALLBACK
// ------------------------------------------------------------

void onTimeSync(struct timeval *tv)
{
    (void)tv;

    ntpHasSynced.store(true, std::memory_order_relaxed);

    enqueueEvent(ManagerEvent::TIME_SYNCED);
}


// ------------------------------------------------------------
// UPDATE NETWORK STATUS
// ------------------------------------------------------------

void publishNetworkStatus()
{
    const bool wifiConnected =
        connected.load(std::memory_order_relaxed);

    const bool timeSynced =
        ntpHasSynced.load(std::memory_order_relaxed);

    char ip[16] = "-";

    if (wifiConnected) {

        const IPAddress address = WiFi.localIP();

        snprintf(
            ip,
            sizeof(ip),
            "%u.%u.%u.%u",
            address[0],
            address[1],
            address[2],
            address[3]
        );
    }

    // The OLED is optional and may currently be disabled.
    if (DisplayManager::isReady()) {

        DisplayManager::setNetworkStatus(
            wifiConnected,
            timeSynced,
            ip
        );
    }
}


// ------------------------------------------------------------
// CONFIGURE NTP
// ------------------------------------------------------------

void configureNTP()
{
    if (ntpConfigured) {
        return;
    }

    // Receive notification after the first successful time sync.
    sntp_set_time_sync_notification_cb(onTimeSync);

    configTzTime(
        TIME_ZONE,
        NTP_SERVER_1,
        NTP_SERVER_2,
        NTP_SERVER_3
    );

    ntpConfigured = true;

    Serial.println("[WIFI] NTP synchronization started");
}


// ------------------------------------------------------------
// WIFI FREERTOS TASK
// ------------------------------------------------------------

void wifiTask(void *parameter)
{
    (void)parameter;

    Serial.println("[WIFI] Manager task started");

    Serial.printf(
        "[WIFI] SSID: %s\n",
        WIFI_SSID
    );

    bool connectionAttempted = false;

    TickType_t lastAttemptTicks = 0;

    constexpr TickType_t retryInterval =
        pdMS_TO_TICKS(RECONNECT_INTERVAL_MS);


    for (;;) {

        // ----------------------------------------------------
        // WAIT FOR WIFI EVENTS OR RECONNECTION TIMEOUT
        // ----------------------------------------------------

        TickType_t waitTicks = portMAX_DELAY;

        if (!connected.load(std::memory_order_relaxed)) {

            if (!connectionAttempted) {

                // First connection attempt.
                waitTicks = 0;
            }
            else {

                const TickType_t now = xTaskGetTickCount();

                const TickType_t elapsed =
                    now - lastAttemptTicks;

                waitTicks =
                    (elapsed >= retryInterval)
                    ? 0
                    : retryInterval - elapsed;
            }
        }


        // ----------------------------------------------------
        // RECEIVE NETWORK EVENT
        // ----------------------------------------------------

        ManagerEvent event;

        if (xQueueReceive(
                wifiEventQueue,
                &event,
                waitTicks) == pdTRUE) {

            switch (event) {

                case ManagerEvent::GOT_IP:
                {
                    connected.store(
                        true,
                        std::memory_order_relaxed
                    );

                    const IPAddress address = WiFi.localIP();

                    Serial.printf(
                        "[WIFI] Connected. IP: %u.%u.%u.%u\n",
                        address[0],
                        address[1],
                        address[2],
                        address[3]
                    );

                    Buzzer::play(Buzzer::Sound::WIFI_CONNECTED);

                    // Configure SNTP only once.
                    configureNTP();

                    publishNetworkStatus();

                    break;
                }


                case ManagerEvent::DISCONNECTED:
                {
                    const bool wasConnected =
                        connected.exchange(
                            false,
                            std::memory_order_relaxed
                        );

                    if (wasConnected) {

                        Serial.println(
                            "[WIFI] Connection lost"
                        );

                        Buzzer::play(
                            Buzzer::Sound::WIFI_DISCONNECTED
                        );
                    }

                    // Wait the configured interval before retrying.
                    connectionAttempted = true;

                    lastAttemptTicks = xTaskGetTickCount();

                    publishNetworkStatus();

                    break;
                }


                case ManagerEvent::TIME_SYNCED:
                {
                    Serial.println(
                        "[WIFI] NTP time synchronized"
                    );

                    publishNetworkStatus();

                    break;
                }
            }
        }


        // ----------------------------------------------------
        // HANDLE CONNECTION / RECONNECTION
        // ----------------------------------------------------

        if (!connected.load(std::memory_order_relaxed)) {

            const TickType_t now = xTaskGetTickCount();

            const bool retryRequired =
                !connectionAttempted ||
                (now - lastAttemptTicks) >= retryInterval;

            if (retryRequired) {

                Serial.println(
                    "[WIFI] Starting connection attempt..."
                );

                Buzzer::play(
                    Buzzer::Sound::WIFI_CONNECTING
                );

                WiFi.begin(
                    WIFI_SSID,
                    WIFI_PASSWORD
                );

                connectionAttempted = true;

                lastAttemptTicks = xTaskGetTickCount();
            }
        }
    }
}

} // namespace


// ============================================================
// PUBLIC FUNCTIONS
// ============================================================


// ------------------------------------------------------------
// INITIALIZE WIFI
// ------------------------------------------------------------

bool begin()
{
    if (initialized) {
        return true;
    }

    wifiEventQueue = xQueueCreate(
        WIFI_EVENT_QUEUE_LENGTH,
        sizeof(ManagerEvent)
    );

    if (wifiEventQueue == nullptr) {
        return false;
    }

    WiFi.persistent(false);

    // Reconnection is managed by our own task.
    WiFi.setAutoReconnect(false);

    // Register callback before starting the station.
    WiFi.onEvent(wifiEventHandler);

    if (!WiFi.mode(WIFI_STA)) {

        vQueueDelete(wifiEventQueue);

        wifiEventQueue = nullptr;

        return false;
    }

    initialized = true;

    return true;
}


// ------------------------------------------------------------
// START WIFI TASK
// ------------------------------------------------------------

bool startTask()
{
    if (!initialized || wifiEventQueue == nullptr ||
        wifiTaskHandle != nullptr) {
        return false;
    }

    if (!hasCredentials()) {

        Serial.println(
            "[WIFI] Missing credentials. "
            "Add arduino_secrets.h"
        );

        return false;
    }

    BaseType_t result = xTaskCreatePinnedToCore(
        wifiTask,
        "WiFiManager",
        WIFI_TASK_STACK_SIZE,
        nullptr,
        WIFI_TASK_PRIORITY,
        &wifiTaskHandle,
        WIFI_TASK_CORE
    );

    if (result != pdPASS) {

        wifiTaskHandle = nullptr;

        return false;
    }

    return true;
}


// ------------------------------------------------------------
// TASK STATUS
// ------------------------------------------------------------

bool isTaskRunning()
{
    return wifiTaskHandle != nullptr;
}


// ------------------------------------------------------------
// CONNECTION STATUS
// ------------------------------------------------------------

bool isConnected()
{
    return connected.load(std::memory_order_relaxed);
}


// ------------------------------------------------------------
// TIME STATUS
// ------------------------------------------------------------

bool isTimeSynced()
{
    return ntpHasSynced.load(std::memory_order_relaxed);
}


bool isTimeValid()
{
    if (!isTimeSynced()) {
        return false;
    }

    const time_t now = time(nullptr);

    return now > 1700000000;
}


// ------------------------------------------------------------
// GET LOCAL TIME
// ------------------------------------------------------------

bool getLocalTime(struct tm &out)
{
    if (!isTimeValid()) {
        return false;
    }

    const time_t now = time(nullptr);

    return localtime_r(&now, &out) != nullptr;
}


// ------------------------------------------------------------
// GET IP ADDRESS
// ------------------------------------------------------------

bool getIPAddress(char *buffer, size_t bufferSize)
{
    if (buffer == nullptr || bufferSize == 0) {
        return false;
    }

    if (!isConnected()) {

        snprintf(buffer, bufferSize, "-");

        return false;
    }

    const IPAddress address = WiFi.localIP();

    snprintf(
        buffer,
        bufferSize,
        "%u.%u.%u.%u",
        address[0],
        address[1],
        address[2],
        address[3]
    );

    return true;
}


// ------------------------------------------------------------
// GET CONNECTED SSID
// ------------------------------------------------------------

bool getSSID(char *buffer, size_t bufferSize)
{
    if (buffer == nullptr || bufferSize == 0) {
        return false;
    }

    if (!isConnected()) {

        snprintf(buffer, bufferSize, "-");

        return false;
    }

    const String currentSSID = WiFi.SSID();

    snprintf(
        buffer,
        bufferSize,
        "%s",
        currentSSID.c_str()
    );

    return true;
}


} // namespace WifiManager