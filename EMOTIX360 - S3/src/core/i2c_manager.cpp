#include "core/i2c_manager.h"

#include "config/pin_config.h"
#include "config/device_config.h"

#include <Wire.h>

namespace I2CManager {
namespace {

bool busReady = false;

}


// ------------------------------------------------------------
// INITIALIZE SHARED I2C BUS
// ------------------------------------------------------------

bool begin()
{
    if (busReady) {
        return true;
    }

    // Initialize ESP32-S3 I2C using the configured GPIO pins.
    busReady = Wire.begin(
        PinConfig::I2C_SDA,
        PinConfig::I2C_SCL,
        I2CConfig::BUS_CLOCK_HZ
    );

    if (!busReady) {
        return false;
    }

    // Prevent indefinite waiting if an I2C transaction fails.
    Wire.setTimeOut(I2CConfig::TIMEOUT_MS);

    return true;
}


// ------------------------------------------------------------
// CHECK I2C INITIALIZATION
// ------------------------------------------------------------

bool isReady()
{
    return busReady;
}


// ------------------------------------------------------------
// CHECK DEVICE ACKNOWLEDGEMENT
// ------------------------------------------------------------

bool isDeviceConnected(uint8_t address)
{
    if (!busReady) {
        return false;
    }

    Wire.beginTransmission(address);

    return Wire.endTransmission() == 0;
}


} // namespace I2CManager