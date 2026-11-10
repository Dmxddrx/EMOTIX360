#include "general.h"

#include "i2c_manager.h"
#include "display_manager.h"
#include "mpu6050_driver.h"

namespace General {

bool begin()
{
    // --------------------------------------------------------
    // INITIALIZE SHARED I2C
    // --------------------------------------------------------

    if (!I2CManager::begin()) {
        return false;
    }


    // --------------------------------------------------------
    // INITIALIZE OLED
    // --------------------------------------------------------

    const bool oledReady = DisplayManager::begin();


    // --------------------------------------------------------
    // INITIALIZE MPU6050
    // --------------------------------------------------------

    const bool mpuReady = MPU6050::begin();


    // --------------------------------------------------------
    // START INDEPENDENT MODULE TASKS
    // --------------------------------------------------------

    // These functions will be implemented in their respective
    // modules. Each module manages its own execution.

    if (mpuReady) {
        MPU6050::startTask();
    }

    if (oledReady) {
        DisplayManager::startTask();
    }

    return true;
}

}