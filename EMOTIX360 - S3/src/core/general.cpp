#include "general.h"

#include "i2c_manager.h"
#include "mpu6050_driver.h"
#include "display_manager.h"

#include <Arduino.h>

namespace General {

bool begin()
{
    // Initialize shared I2C.
    if (!I2CManager::begin()) {
        Serial.println("I2C initialization failed");
        return false;
    }

    Serial.println("I2C ready");


    // Initialize hardware.
    const bool oledReady = DisplayManager::begin();
    const bool mpuReady = MPU6050::begin();

    if (oledReady) {
        DisplayManager::showStartupMessage("EMOTIX360");
    }


    // Start independent MPU task.
    const bool mpuTaskStarted =
        mpuReady && MPU6050::startTask();


    // Prepare initial display state.
    if (oledReady) {

        DisplayManager::Status status{};

        status.mpuReady = mpuTaskStarted;

        // modeName already defaults to EMOTIX360.
        DisplayManager::setStatus(status);
    }


    // Start independent OLED task.
    const bool oledTaskStarted =
        oledReady && DisplayManager::startTask();


    // Report initialization.
    Serial.printf(
        "MPU6050: %s\n",
        mpuTaskStarted ? "RUNNING" : "FAILED"
    );

    Serial.printf(
        "OLED: %s\n",
        oledTaskStarted ? "RUNNING" : "FAILED"
    );

    return mpuTaskStarted && oledTaskStarted;
}

}