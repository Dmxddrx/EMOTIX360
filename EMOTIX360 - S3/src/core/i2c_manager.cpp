#include "i2c_manager.h"

#include "pin_config.h"
#include "device_config.h"

#include <Wire.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace I2CManager {
namespace {

SemaphoreHandle_t busMutex = nullptr;
bool busReady = false;

}


// ------------------------------------------------------------
// INITIALIZATION
// ------------------------------------------------------------

bool begin()
{
    if (busReady) {
        return true;
    }

    busMutex = xSemaphoreCreateMutex();

    if (busMutex == nullptr) {
        return false;
    }

    busReady = Wire.begin(
        PinConfig::I2C_SDA,
        PinConfig::I2C_SCL,
        I2CConfig::BUS_CLOCK_HZ
    );

    if (!busReady) {
        vSemaphoreDelete(busMutex);
        busMutex = nullptr;
        return false;
    }

    Wire.setTimeOut(I2CConfig::TIMEOUT_MS);

    return true;
}


// ------------------------------------------------------------
// STATUS
// ------------------------------------------------------------

bool isReady()
{
    return busReady;
}


// ------------------------------------------------------------
// BUS SYNCHRONIZATION
// ------------------------------------------------------------

bool lock(uint32_t timeoutMs)
{
    if (!busReady || busMutex == nullptr) {
        return false;
    }

    return xSemaphoreTake(
        busMutex,
        pdMS_TO_TICKS(timeoutMs)
    ) == pdTRUE;
}


void unlock()
{
    if (busMutex != nullptr) {
        xSemaphoreGive(busMutex);
    }
}


// ------------------------------------------------------------
// DEVICE DETECTION
// ------------------------------------------------------------

bool isDeviceConnected(uint8_t address)
{
    if (!lock()) {
        return false;
    }

    Wire.beginTransmission(address);

    const bool connected =
        Wire.endTransmission() == 0;

    unlock();

    return connected;
}

} // namespace I2CManager