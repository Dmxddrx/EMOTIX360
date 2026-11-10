#ifndef MPU6050_DRIVER_H
#define MPU6050_DRIVER_H

#include <Arduino.h>

namespace MPU6050 {

struct Data {

    // Raw accelerometer readings.
    int16_t axRaw = 0;
    int16_t ayRaw = 0;
    int16_t azRaw = 0;

    // Raw temperature.
    int16_t temperatureRaw = 0;

    // Raw gyroscope readings.
    int16_t gxRaw = 0;
    int16_t gyRaw = 0;
    int16_t gzRaw = 0;

    // Converted acceleration in g.
    float axG = 0.0f;
    float ayG = 0.0f;
    float azG = 0.0f;

    // Converted angular velocity in degrees/second.
    float gxDps = 0.0f;
    float gyDps = 0.0f;
    float gzDps = 0.0f;

    float temperatureC = 0.0f;

    // Complementary filter angles.
    float pitchDeg = 0.0f;
    float rollDeg = 0.0f;

    uint32_t timestampMs = 0;

    bool valid = false;
};

bool begin();
void update();

bool isReady();
uint8_t address();

const Data &getData();

}

#endif