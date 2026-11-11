#include "general.h"

#include "i2c_manager.h"
#include "mpu6050.h"
#include "display_manager.h"
#include "device_config.h"
#include "wifi_manager.h"
#include "max7219.h"
#include "animation_manager.h"
#include "button.h"

#include <Arduino.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace General {
namespace {

constexpr uint32_t SERIAL_TASK_STACK_SIZE = 4096;
constexpr UBaseType_t SERIAL_TASK_PRIORITY = 1;

constexpr uint32_t SERIAL_REFRESH_MS = 500;

TaskHandle_t serialTaskHandle = nullptr;


// ------------------------------------------------------------
// SERIAL DIAGNOSTIC TASK
// ------------------------------------------------------------

void serialMonitorTask(void *parameter)
{
    MPU6050::Data imu{};

    Serial.println();
    Serial.println("==========================================");
    Serial.println("          EMOTIX360 DIAGNOSTICS");
    Serial.println("==========================================");

    for (;;) {

        if (!MPU6050::isReady() ||
            !MPU6050::isTaskRunning()) {

            Serial.println("[MPU6050] INITIALIZATION FAILED");
        }
        else if (!MPU6050::getData(imu)) {

            Serial.println("[MPU6050] WAITING FOR SENSOR DATA");
        }
        else {

            const uint32_t ageMs =
                millis() - imu.timestampMs;

            if (ageMs > 1000) {

                Serial.printf(
                    "[MPU6050] WARNING: STALE DATA (%lu ms)\n",
                    (unsigned long)ageMs
                );
            }
            else {

                Serial.printf(
                    "[RAW] AX:%6d AY:%6d AZ:%6d | "
                    "GX:%6d GY:%6d GZ:%6d\n",
                    imu.axRaw,
                    imu.ayRaw,
                    imu.azRaw,
                    imu.gxRaw,
                    imu.gyRaw,
                    imu.gzRaw
                );

                Serial.printf(
                    "[ACC] X:%+.3f Y:%+.3f Z:%+.3f g\n",
                    imu.axG,
                    imu.ayG,
                    imu.azG
                );

                Serial.printf(
                    "[GYR] X:%+.2f Y:%+.2f Z:%+.2f deg/s\n",
                    imu.gxDps,
                    imu.gyDps,
                    imu.gzDps
                );

                Serial.printf(
                    "[IMU] Pitch:%+.2f Roll:%+.2f Temp:%.2f C\n",
                    imu.pitchDeg,
                    imu.rollDeg,
                    imu.temperatureC
                );

                Serial.printf(
                    "[SYSTEM] Sample age:%lu ms | MPU:%s\n",
                    (unsigned long)ageMs,
                    imu.valid ? "VALID" : "INVALID"
                );
            }
        }

        Serial.println("------------------------------------------");

        vTaskDelay(pdMS_TO_TICKS(SERIAL_REFRESH_MS));
    }
}


// ------------------------------------------------------------
// START SERIAL MONITOR TASK
// ------------------------------------------------------------

bool startSerialTask()
{
    if (serialTaskHandle != nullptr) {
        return false;
    }

    return xTaskCreate(
        serialMonitorTask,
        "SerialMonitor",
        SERIAL_TASK_STACK_SIZE,
        nullptr,
        SERIAL_TASK_PRIORITY,
        &serialTaskHandle
    ) == pdPASS;
}

} // namespace


// ------------------------------------------------------------
// INITIALIZE EMOTIX360
// ------------------------------------------------------------

bool begin()
{
    Serial.println();
    Serial.println("[GENERAL] Initializing EMOTIX360");


    // Initialize shared I2C.
    if (!I2CManager::begin()) {
        Serial.println("[GENERAL] I2C initialization failed");
        return false;
    }

    Serial.println("[GENERAL] I2C ready");


    // --------------------------------------------------------
    // INITIALIZE MAX7219 MATRIX
    // --------------------------------------------------------

    const bool matrixReady = MAX7219_Init();

    Serial.printf(
        "[GENERAL] MAX7219: %s\n",
        matrixReady ? "INITIALIZED" : "FAILED"
    );

    if (matrixReady) {

        MAX7219_Clear();

        // Draw the outer boundary of both 8x8 modules.
        for (int16_t x = 0; x < 16; ++x) {
            MAX7219_SetPixel(x, 0, 1);
            MAX7219_SetPixel(x, 7, 1);
        }

        for (int16_t y = 0; y < 8; ++y) {

            MAX7219_SetPixel(0, y, 1);
            MAX7219_SetPixel(7, y, 1);

            MAX7219_SetPixel(8, y, 1);
            MAX7219_SetPixel(15, y, 1);
        }

        MAX7219_Update();
    }

    // Initialize OLED only when enabled.
    if (DeviceConfig::OLED_ENABLED) {

        if (DisplayManager::begin()) {

            Serial.println("[GENERAL] OLED ready");

            DisplayManager::showStartupMessage("EMOTIX360");
        }
        else {

            Serial.println("[GENERAL] OLED initialization failed");
        }
    }
    else {

        Serial.println("[GENERAL] OLED disabled");
    }


    // Initialize MPU6050.
    const bool mpuReady = MPU6050::begin();

    if (mpuReady) {

        Serial.printf(
            "[GENERAL] MPU6050 detected at 0x%02X\n",
            MPU6050::address()
        );
    }
    else {

        Serial.println("[GENERAL] MPU6050 initialization failed");
    }


    // Start MPU6050 task.
    bool mpuTaskStarted = false;

    if (mpuReady) {
        mpuTaskStarted = MPU6050::startTask();
    }

    Serial.printf(
        "[GENERAL] MPU task: %s\n",
        mpuTaskStarted ? "STARTED" : "FAILED"
    );

    // ------------------------------------------------------------
    // INITIALIZE ANIMATION MANAGER AND BUTTON
    // ------------------------------------------------------------

    // Keep this initial mode identical in both managers.
    constexpr AnimationManager::Mode INITIAL_ANIMATION_MODE =
        AnimationManager::Mode::SAND_GRAVITY;

    // Change to Mode::SAND_CLOCK if you prefer SandClock at startup.

    bool animationReady = false;
    bool buttonTaskStarted = false;

    if (matrixReady && mpuTaskStarted)
    {
        if (AnimationManager::begin(INITIAL_ANIMATION_MODE))
        {
            animationReady = AnimationManager::startTask();
        }

        if (animationReady)
        {
            if (ButtonManager::begin(INITIAL_ANIMATION_MODE))
            {
                buttonTaskStarted = ButtonManager::startTask();
            }
        }
    }

    Serial.printf(
        "[GENERAL] Animation Manager: %s\n",
        animationReady ? "STARTED" : "FAILED"
    );

    Serial.printf(
        "[GENERAL] Button Manager: %s\n",
        buttonTaskStarted ? "STARTED" : "FAILED"
    );

    // INITIALIZE WIFI MANAGER
    const bool wifiInitialized = WifiManager::begin();

    bool wifiTaskStarted = false;

    if (wifiInitialized) {
        wifiTaskStarted = WifiManager::startTask();
    }

    Serial.printf(
        "[GENERAL] WiFi Manager: %s\n",
        wifiTaskStarted ? "STARTED" : "UNAVAILABLE"
    );


    // Start OLED task only when OLED is enabled.
    bool oledTaskStarted = false;

    if (DeviceConfig::OLED_ENABLED &&
        DisplayManager::isReady()) {

        DisplayManager::setApplicationStatus(
            0,
            "EMOTIX360"
        );

        DisplayManager::setMPUStatus(
            mpuTaskStarted,
            0.0f,
            0.0f
        );

        oledTaskStarted = DisplayManager::startTask();
    }

    Serial.printf(
        "[GENERAL] OLED task: %s\n",
        oledTaskStarted ? "STARTED" :
        (DeviceConfig::OLED_ENABLED ? "FAILED" : "DISABLED")
    );


    // Start independent Serial diagnostic task.
    const bool serialTaskStarted = startSerialTask();

    Serial.printf(
        "[GENERAL] Serial monitor: %s\n",
        serialTaskStarted ? "STARTED" : "FAILED"
    );


    Serial.println("==========================================");

    return mpuTaskStarted &&
       serialTaskStarted &&
       matrixReady &&
       animationReady &&
       buttonTaskStarted &&
       (!DeviceConfig::OLED_ENABLED || oledTaskStarted);
}

} // namespace General