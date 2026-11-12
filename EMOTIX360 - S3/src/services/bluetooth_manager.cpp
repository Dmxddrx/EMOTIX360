
#include "bluetooth_manager.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include <Arduino.h>
#include <string.h>
#include <stdio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/portmacro.h>


namespace BluetoothManager {
namespace {

// ============================================================
// BLE CONFIGURATION
// ============================================================

// Advertised device name.
constexpr char DEVICE_NAME[] = "EMOTIX360";

// Preserve the original C3 UUIDs for mobile-app compatibility.
constexpr char SERVICE_UUID[] =
    "4fafc201-1fb5-459e-8fcc-c5c9c331914b";

constexpr char COMMAND_UUID[] =
    "beb5483e-36e1-4688-b7f5-ea07361b26a8";

constexpr char STATUS_UUID[] =
    "e3223119-9445-4e96-a4a1-85358c4046a2";


// ============================================================
// FREERTOS CONFIGURATION
// ============================================================

constexpr uint32_t BLE_TASK_STACK_SIZE = 4096;

constexpr UBaseType_t BLE_TASK_PRIORITY = 1;

constexpr uint32_t BLE_UPDATE_INTERVAL_MS = 1000;


// ============================================================
// BLE OBJECTS
// ============================================================

BLEServer *server = nullptr;

BLEService *service = nullptr;

BLECharacteristic *commandCharacteristic = nullptr;

BLECharacteristic *statusCharacteristic = nullptr;


// ============================================================
// INTERNAL STATE
// ============================================================

bool initialized = false;

TaskHandle_t bluetoothTaskHandle = nullptr;

// Set when a client connection has been detected.
// Used to restart advertising after disconnection.
bool previousConnectionState = false;


// ============================================================
// COMMAND BUFFER
// ============================================================

constexpr size_t COMMAND_BUFFER_SIZE = 48;

portMUX_TYPE commandMux =
    portMUX_INITIALIZER_UNLOCKED;

char pendingCommand[COMMAND_BUFFER_SIZE] = {};

bool commandPending = false;


// ------------------------------------------------------------
// ENQUEUE RECEIVED COMMAND
// ------------------------------------------------------------

/*
 * BLE callbacks execute in a different task context.
 *
 * This function safely copies the received command into
 * the application command buffer.
 *
 * If multiple commands arrive before takeCommand(), the
 * newest command replaces the previous pending command.
 */
void enqueueCommand(const char *data, size_t length)
{
    if (data == nullptr || length == 0) {
        return;
    }

    if (length >= COMMAND_BUFFER_SIZE) {
        length = COMMAND_BUFFER_SIZE - 1;
    }

    portENTER_CRITICAL(&commandMux);

    memcpy(
        pendingCommand,
        data,
        length
    );

    pendingCommand[length] = '\0';

    commandPending = true;

    portEXIT_CRITICAL(&commandMux);
}


// ============================================================
// BLE CALLBACKS
// ============================================================

// ------------------------------------------------------------
// COMMAND CHARACTERISTIC CALLBACK
// ------------------------------------------------------------

class CommandCallbacks final :
    public BLECharacteristicCallbacks {

    void onWrite(
        BLECharacteristic *characteristic
    ) override
    {
        if (characteristic == nullptr) {
            return;
        }

        const String value =
            characteristic->getValue();

        if (value.length() == 0) {
            return;
        }

        enqueueCommand(
            value.c_str(),
            value.length()
        );

        Serial.printf(
            "[BLE] Command received: %s\n",
            value.c_str()
        );
    }
};


// ============================================================
// BLUETOOTH FREERTOS TASK
// ============================================================

/*
 * The current application loop sleeps instead of continuously
 * calling update().
 *
 * This independent task performs periodic BLE maintenance.
 *
 * It does not consume commands. The application can retrieve
 * them separately through takeCommand().
 */
void bluetoothTask(void *parameter)
{
    (void)parameter;

    Serial.println("[BLE] Manager task started");

    TickType_t lastWakeTime =
        xTaskGetTickCount();

    TickType_t updatePeriod =
        pdMS_TO_TICKS(BLE_UPDATE_INTERVAL_MS);

    if (updatePeriod == 0) {
        updatePeriod = 1;
    }

    for (;;)
    {
        update();

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
// INITIALIZE BLUETOOTH
// ------------------------------------------------------------

bool begin()
{
    if (initialized) {
        return true;
    }

    Serial.println();
    Serial.println("[BLE] Initializing Bluetooth LE");


    // --------------------------------------------------------
    // INITIALIZE BLE STACK
    // --------------------------------------------------------

    BLEDevice::init(DEVICE_NAME);

    // Request a larger MTU for future command extensions.
    BLEDevice::setMTU(100);

    Serial.printf(
        "[BLE] Device name: %s\n",
        DEVICE_NAME
    );


    // --------------------------------------------------------
    // CREATE BLE SERVER
    // --------------------------------------------------------

    server = BLEDevice::createServer();

    if (server == nullptr)
    {
        Serial.println(
            "[BLE] Server creation failed"
        );

        return false;
    }


    // --------------------------------------------------------
    // CREATE GATT SERVICE
    // --------------------------------------------------------

    service = server->createService(
        SERVICE_UUID
    );

    if (service == nullptr)
    {
        Serial.println(
            "[BLE] Service creation failed"
        );

        return false;
    }


    // --------------------------------------------------------
    // COMMAND CHARACTERISTIC
    // --------------------------------------------------------

    commandCharacteristic =
        service->createCharacteristic(
            COMMAND_UUID,
            BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_WRITE
        );

    if (commandCharacteristic == nullptr)
    {
        Serial.println(
            "[BLE] Command characteristic creation failed"
        );

        return false;
    }

    // Initial readable value.
    commandCharacteristic->setValue(
        String("MODE=WATER")
    );

    commandCharacteristic->setCallbacks(
        new CommandCallbacks()
    );


    // --------------------------------------------------------
    // STATUS CHARACTERISTIC
    // --------------------------------------------------------

    statusCharacteristic =
        service->createCharacteristic(
            STATUS_UUID,
            BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_NOTIFY
        );

    if (statusCharacteristic == nullptr)
    {
        Serial.println(
            "[BLE] Status characteristic creation failed"
        );

        return false;
    }

    // Required for client notification subscription.
    statusCharacteristic->addDescriptor(
        new BLE2902()
    );

    statusCharacteristic->setValue(
        String("booting")
    );


    // --------------------------------------------------------
    // START GATT SERVICE
    // --------------------------------------------------------

    service->start();

    Serial.println(
        "[BLE] GATT service started"
    );


    // --------------------------------------------------------
    // CONFIGURE BLE ADVERTISING
    // --------------------------------------------------------

    BLEAdvertising *advertising =
        BLEDevice::getAdvertising();

    if (advertising == nullptr)
    {
        Serial.println(
            "[BLE] Advertising initialization failed"
        );

        return false;
    }

    advertising->addServiceUUID(
        SERVICE_UUID
    );

    advertising->setScanResponse(true);

    advertising->start();


    // --------------------------------------------------------
    // COMPLETE INITIALIZATION
    // --------------------------------------------------------

    previousConnectionState = false;

    initialized = true;

    Serial.println(
        "[BLE] Advertising started"
    );

    Serial.printf(
        "[BLE] Search for: %s\n",
        DEVICE_NAME
    );

    return true;
}


// ------------------------------------------------------------
// START BLUETOOTH TASK
// ------------------------------------------------------------

bool startTask()
{
    if (!initialized) {

        Serial.println(
            "[BLE] Cannot start task: manager not initialized"
        );

        return false;
    }

    if (bluetoothTaskHandle != nullptr) {
        return true;
    }

    const BaseType_t result = xTaskCreate(
        bluetoothTask,
        "BluetoothTask",
        BLE_TASK_STACK_SIZE,
        nullptr,
        BLE_TASK_PRIORITY,
        &bluetoothTaskHandle
    );

    if (result != pdPASS)
    {
        bluetoothTaskHandle = nullptr;

        Serial.println(
            "[BLE] Failed to create Bluetooth task"
        );

        return false;
    }

    Serial.println(
        "[BLE] Manager task created"
    );

    return true;
}


// ------------------------------------------------------------
// UPDATE BLUETOOTH
// ------------------------------------------------------------

/*
 * Detect connection state changes.
 *
 * Initial state:
 *     Advertising is already active.
 *
 * Client connects:
 *     Record the connected state.
 *
 * Client disconnects:
 *     Restart advertising once.
 *
 * This avoids repeatedly restarting advertising every
 * few seconds when no device is connected.
 */
void update()
{
    if (!initialized || server == nullptr) {
        return;
    }

    const bool connectedNow =
        server->getConnectedCount() > 0;

    if (connectedNow)
    {
        previousConnectionState = true;
        return;
    }

    // A client has disconnected.
    if (previousConnectionState)
    {
        previousConnectionState = false;

        BLEDevice::startAdvertising();

        Serial.println(
            "[BLE] Client disconnected. Advertising restarted"
        );
    }
}


// ------------------------------------------------------------
// CONNECTION STATUS
// ------------------------------------------------------------

bool isConnected()
{
    return (
        server != nullptr &&
        server->getConnectedCount() > 0
    );
}


// ------------------------------------------------------------
// TASK STATUS
// ------------------------------------------------------------

bool isTaskRunning()
{
    return bluetoothTaskHandle != nullptr;
}


// ------------------------------------------------------------
// PUBLISH DEVICE STATUS
// ------------------------------------------------------------

/*
 * Status format remains compatible with the previous
 * WaterMatrixClock implementation.
 *
 * Format:
 *
 * P=12.3,R=-4.5,WIFI=1,TIME=1,MODE=HEART,BRIGHT=5
 *
 * The mobile application can read this characteristic
 * or subscribe to notifications.
 */
void setStatus(
    float pitchDeg,
    float rollDeg,
    bool wifiConnected,
    bool timeValid,
    const char *modeName,
    uint8_t brightness
)
{
    if (statusCharacteristic == nullptr) {
        return;
    }

    char status[100];

    snprintf(
        status,
        sizeof(status),
        "P=%.1f,R=%.1f,WIFI=%d,TIME=%d,MODE=%s,BRIGHT=%u",
        pitchDeg,
        rollDeg,
        wifiConnected ? 1 : 0,
        timeValid ? 1 : 0,
        (modeName != nullptr) ? modeName : "?",
        brightness
    );

    // Update the readable characteristic value.
    statusCharacteristic->setValue(
        String(status)
    );

    // Send notifications only to an active BLE client.
    if (isConnected())
    {
        statusCharacteristic->notify();
    }
}


// ------------------------------------------------------------
// RETRIEVE RECEIVED COMMAND
// ------------------------------------------------------------

/*
 * Retrieve the latest pending command.
 *
 * Returns:
 *     true  = A command was received.
 *     false = No pending command.
 *
 * The command buffer is cleared after retrieval.
 */
bool takeCommand(
    char *out,
    size_t outSize
)
{
    if (out == nullptr || outSize == 0) {
        return false;
    }

    bool received = false;

    portENTER_CRITICAL(&commandMux);

    if (commandPending)
    {
        strncpy(
            out,
            pendingCommand,
            outSize - 1
        );

        out[outSize - 1] = '\0';

        commandPending = false;

        received = true;
    }

    portEXIT_CRITICAL(&commandMux);

    return received;
}

} // namespace BluetoothManager
