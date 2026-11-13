#include "general.h"

#include "i2c_manager.h"
#include "mpu6050.h"
#include "display_manager.h"
#include "device_config.h"
#include "wifi_manager.h"
#include "max7219.h"
#include "animation_manager.h"
#include "button.h"
#include "bluetooth_manager.h"
#include "buzzer.h"

#include <Arduino.h>

namespace General {
namespace {

// ------------------------------------------------------------
// SERIAL DIAGNOSTIC TASK
// ------------------------------------------------------------

void printSerialDiagnosticHeader()
{
    Serial.println();
    Serial.println("          EMOTIX360 DIAGNOSTICS");
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


    // INITIALIZE MAX7219 MATRIX
    const bool matrixReady = MAX7219_Init();

    Serial.printf(
        "[GENERAL] MAX7219: %s\n",
        matrixReady ? "INITIALIZED" : "FAILED"
    );

    if (matrixReady)
    {
        // Start with a clean framebuffer.
        // SmallAnimations will perform the startup border effect.
        MAX7219_Clear();
        MAX7219_Update();
    }


    // INITIALIZE BUZZER MANAGER
    const bool buzzerInitialized = Buzzer::begin();

    bool buzzerTaskStarted = false;

    if (buzzerInitialized) {
        buzzerTaskStarted = Buzzer::startTask();
    }

    Serial.printf(
        "[GENERAL] Buzzer Manager: %s\n",
        buzzerTaskStarted ? "STARTED" : "FAILED"
    );

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

    // The animation manager starts with the one-time boot animation.
    constexpr AnimationManager::Mode INITIAL_ANIMATION_MODE =
        AnimationManager::Mode::STARTUP;

    // The button sequence starts at HEART.
    // Therefore, the first button press selects RAIN.
    // STARTUP is not part of the button-controlled sequence.
    constexpr AnimationManager::Mode INITIAL_BUTTON_MODE =
        AnimationManager::Mode::HEART;

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
            if (ButtonManager::begin(INITIAL_BUTTON_MODE))
            {
                buttonTaskStarted = ButtonManager::startTask();
            }
        }
    }

    if (buzzerTaskStarted && animationReady) {
        Buzzer::play(Buzzer::Sound::STARTUP);
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


    // INITIALIZE BLUETOOTH MANAGER
    const bool bluetoothInitialized =
        BluetoothManager::begin();

    bool bluetoothTaskStarted = false;

    if (bluetoothInitialized)
    {
        bluetoothTaskStarted =
            BluetoothManager::startTask();
    }

    Serial.printf(
        "[GENERAL] Bluetooth Manager: %s\n",
        bluetoothTaskStarted ? "STARTED" : "FAILED"
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


    // --------------------------------------------------------
    // SERIAL DIAGNOSTICS
    // --------------------------------------------------------

    // No separate Serial Monitor task is needed.
    // Other modules continue printing their own diagnostics.

    Serial.println("[GENERAL] Serial monitor: STARTED");

    printSerialDiagnosticHeader();

    // INITIALIZATION RESULT
    return mpuTaskStarted &&
       matrixReady &&
       animationReady &&
       buttonTaskStarted &&
       (!DeviceConfig::OLED_ENABLED || oledTaskStarted);
}

} // namespace General