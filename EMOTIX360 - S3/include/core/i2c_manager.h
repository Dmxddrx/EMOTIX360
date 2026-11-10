#ifndef I2C_MANAGER_H
#define I2C_MANAGER_H

#include <Arduino.h>

namespace I2CManager {

bool begin();
bool isReady();

bool lock(uint32_t timeoutMs = 50);
void unlock();

bool isDeviceConnected(uint8_t address);

}

#endif