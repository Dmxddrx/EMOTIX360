#ifndef I2C_MANAGER_H
#define I2C_MANAGER_H

#include <Arduino.h>

namespace I2CManager {

// Initialize the shared I2C bus.
bool begin();

// Check whether the bus has been initialized.
bool isReady();

// Check whether a device acknowledges an I2C address.
bool isDeviceConnected(uint8_t address);

}

#endif