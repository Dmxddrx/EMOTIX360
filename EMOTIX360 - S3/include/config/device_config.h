#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <Arduino.h>

namespace I2CConfig {

constexpr uint32_t BUS_CLOCK_HZ = 100000UL;
constexpr uint32_t OLED_TRANSFER_HZ = 400000UL;

constexpr uint16_t TIMEOUT_MS = 50;

}

namespace DeviceConfig {

// SSD1306 OLED
constexpr uint8_t OLED_ADDRESS = 0x3C;

// MPU6050
constexpr uint8_t MPU6050_ADDRESS_LOW  = 0x68;
constexpr uint8_t MPU6050_ADDRESS_HIGH = 0x69;

}

#endif