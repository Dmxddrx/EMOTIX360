#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

#include <Arduino.h>

namespace PinConfig {

// I2C
constexpr uint8_t I2C_SDA = 6;
constexpr uint8_t I2C_SCL = 7;

// MPU6050 interrupt
constexpr uint8_t MPU6050_INT = 4;

// MAX7219 SPI
constexpr uint8_t MATRIX_CLK  = 12;
constexpr uint8_t MATRIX_MOSI = 11;
constexpr uint8_t MATRIX_CS   = 10;

// Buttons
// Animation selection tactile button.
// Active LOW: button connects GPIO14 to GND when pressed.
constexpr uint8_t BUTTON_ANIMATION = 14;

}

#endif