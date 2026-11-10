#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

#include <Arduino.h>

namespace PinConfig {

// ------------------------------------------------------------
// ESP32-S3 I2C PINS
// ------------------------------------------------------------

constexpr uint8_t I2C_SDA = 6;
constexpr uint8_t I2C_SCL = 7;

constexpr uint8_t MPU6050_INT = 4;

}

#endif