#include "drivers/mpu6050_driver.h"

#include "core/i2c_manager.h"
#include "config/device_config.h"

#include <Wire.h>
#include <math.h>

namespace MPU6050 {
namespace {

// ------------------------------------------------------------
// MPU6050 REGISTER DEFINITIONS
// ------------------------------------------------------------

constexpr uint8_t REG_SMPLRT_DIV   = 0x19;
constexpr uint8_t REG_CONFIG       = 0x1A;
constexpr uint8_t REG_GYRO_CONFIG  = 0x1B;
constexpr uint8_t REG_ACCEL_CONFIG = 0x1C;

constexpr uint8_t REG_ACCEL_XOUT_H = 0x3B;

constexpr uint8_t REG_PWR_MGMT_1   = 0x6B;
constexpr uint8_t REG_WHO_AM_I     = 0x75;


// ------------------------------------------------------------
// SENSOR CONVERSION CONSTANTS
// ------------------------------------------------------------

// Accelerometer: +/-2 g.
constexpr float ACCEL_SCALE_G = 1.0f / 16384.0f;

// Gyroscope: +/-250 degrees/second.
constexpr float GYRO_SCALE_DPS = 1.0f / 131.0f;

constexpr float RAD_TO_DEG_F = 57.2957795f;


// ------------------------------------------------------------
// GYROSCOPE CALIBRATION
// ------------------------------------------------------------

constexpr uint16_t CALIBRATION_SAMPLES = 100;

// Require at least 80 successful readings.
constexpr uint16_t CALIBRATION_MIN_SAMPLES = 80;

constexpr uint16_t CALIBRATION_DELAY_MS = 5;


// ------------------------------------------------------------
// DRIVER STATE
// ------------------------------------------------------------

bool ready = false;

uint8_t deviceAddress = DeviceConfig::MPU6050_ADDRESS_LOW;

Data latest{};

float gyroBiasX = 0.0f;
float gyroBiasY = 0.0f;
float gyroBiasZ = 0.0f;

uint32_t lastSampleMs = 0;

bool filterInitialized = false;


// ------------------------------------------------------------
// WRITE MPU6050 REGISTER
// ------------------------------------------------------------

bool writeRegister(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(deviceAddress);

    Wire.write(reg);
    Wire.write(value);

    return Wire.endTransmission() == 0;
}


// ------------------------------------------------------------
// READ MULTIPLE MPU6050 REGISTERS
// ------------------------------------------------------------

bool readRegisters(
    uint8_t reg,
    uint8_t *buffer,
    size_t length
)
{
    Wire.beginTransmission(deviceAddress);

    Wire.write(reg);

    // Repeated start: keep the bus active for the read.
    if (Wire.endTransmission(false) != 0) {
        return false;
    }

    const size_t received = Wire.requestFrom(
        deviceAddress,
        length,
        true
    );

    if (received != length) {

        // Clear any incomplete response.
        while (Wire.available()) {
            (void)Wire.read();
        }

        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        buffer[i] = (uint8_t)Wire.read();
    }

    return true;
}


// ------------------------------------------------------------
// CONVERT TWO BYTES INTO SIGNED 16-BIT VALUE
// ------------------------------------------------------------

int16_t readSigned16(
    const uint8_t highByte,
    const uint8_t lowByte
)
{
    return (int16_t)(
        ((uint16_t)highByte << 8) | lowByte
    );
}


// ------------------------------------------------------------
// READ COMPLETE RAW SENSOR DATA
// ------------------------------------------------------------

bool readRaw(Data &out)
{
    uint8_t buffer[14];

    if (!readRegisters(
            REG_ACCEL_XOUT_H,
            buffer,
            sizeof(buffer))) {
        return false;
    }

    out.axRaw = readSigned16(buffer[0], buffer[1]);
    out.ayRaw = readSigned16(buffer[2], buffer[3]);
    out.azRaw = readSigned16(buffer[4], buffer[5]);

    out.temperatureRaw = readSigned16(buffer[6], buffer[7]);

    out.gxRaw = readSigned16(buffer[8], buffer[9]);
    out.gyRaw = readSigned16(buffer[10], buffer[11]);
    out.gzRaw = readSigned16(buffer[12], buffer[13]);

    return true;
}


// ------------------------------------------------------------
// GYROSCOPE BIAS CALIBRATION
// ------------------------------------------------------------

// Keep the device stationary during calibration.
//
// The average stationary gyroscope readings become the bias
// values, which are subtracted from subsequent measurements.

bool calibrateGyro()
{
    int64_t sx = 0;
    int64_t sy = 0;
    int64_t sz = 0;

    uint16_t good = 0;

    for (
        uint16_t i = 0;
        i < CALIBRATION_SAMPLES;
        ++i
    ) {
        Data sample;

        if (readRaw(sample)) {

            sx += sample.gxRaw;
            sy += sample.gyRaw;
            sz += sample.gzRaw;

            ++good;
        }

        delay(CALIBRATION_DELAY_MS);
        yield();
    }

    // Do not accept calibration with too few successful reads.
    if (good < CALIBRATION_MIN_SAMPLES) {
        return false;
    }

    gyroBiasX = (float)sx / good;
    gyroBiasY = (float)sy / good;
    gyroBiasZ = (float)sz / good;

    return true;
}

} // namespace


// ============================================================
// PUBLIC DRIVER FUNCTIONS
// ============================================================


// ------------------------------------------------------------
// INITIALIZE MPU6050
// ------------------------------------------------------------

bool begin()
{
    ready = false;

    // I2C must be initialized before starting the sensor.
    if (!I2CManager::isReady()) {
        return false;
    }

    // Reset previous sensor state.
    latest = Data{};

    filterInitialized = false;

    gyroBiasX = 0.0f;
    gyroBiasY = 0.0f;
    gyroBiasZ = 0.0f;

    uint8_t id = 0;

    bool found = false;


    // --------------------------------------------------------
    // DETECT MPU6050 ADDRESS
    // --------------------------------------------------------

    const uint8_t candidates[] = {
        DeviceConfig::MPU6050_ADDRESS_LOW,
        DeviceConfig::MPU6050_ADDRESS_HIGH
    };

    for (uint8_t addr : candidates) {

        deviceAddress = addr;

        if (readRegisters(REG_WHO_AM_I, &id, 1) &&
            (id == 0x68 ||
             id == 0x70 ||
             id == 0x71 ||
             id == 0x73)) {

            found = true;
            break;
        }
    }

    if (!found) {
        return false;
    }


    // --------------------------------------------------------
    // RESET MPU6050
    // --------------------------------------------------------

    // Reset, wake with the X-gyro PLL, then configure a 100 Hz
    // sample rate.

    if (!writeRegister(REG_PWR_MGMT_1, 0x80)) {
        return false;
    }

    delay(100);


    // --------------------------------------------------------
    // CONFIGURE SENSOR
    // --------------------------------------------------------

    // Sample rate:
    // 1000 / (1 + 9) = 100 Hz
    //
    // Accelerometer: +/-2 g
    // Gyroscope:     +/-250 degrees/second
    // DLPF:          Configuration 3

    if (!writeRegister(REG_PWR_MGMT_1, 0x01) ||
        !writeRegister(REG_SMPLRT_DIV, 0x09) ||
        !writeRegister(REG_CONFIG, 0x03) ||
        !writeRegister(REG_GYRO_CONFIG, 0x00) ||
        !writeRegister(REG_ACCEL_CONFIG, 0x00)) {

        return false;
    }

    delay(50);


    // --------------------------------------------------------
    // CALIBRATE GYROSCOPE
    // --------------------------------------------------------

    if (!calibrateGyro()) {
        return false;
    }


    // --------------------------------------------------------
    // INITIALIZE FILTER AND TIMING
    // --------------------------------------------------------

    latest = Data{};

    filterInitialized = false;

    lastSampleMs = millis();

    ready = true;

    return true;
}


// ------------------------------------------------------------
// UPDATE SENSOR DATA
// ------------------------------------------------------------

void update()
{
    if (!ready) {
        return;
    }

    const uint32_t now = millis();

    if ((uint32_t)(now - lastSampleMs) <
        DeviceConfig::SENSOR_SAMPLE_MS) {
        return;
    }

    Data sample;

    if (!readRaw(sample)) {

        latest.valid = false;

        return;
    }


    // --------------------------------------------------------
    // ACCELEROMETER CONVERSION
    // --------------------------------------------------------

    sample.axG = sample.axRaw * ACCEL_SCALE_G;
    sample.ayG = sample.ayRaw * ACCEL_SCALE_G;
    sample.azG = sample.azRaw * ACCEL_SCALE_G;


    // --------------------------------------------------------
    // GYROSCOPE CONVERSION AND BIAS CORRECTION
    // --------------------------------------------------------

    sample.gxDps =
        (sample.gxRaw - gyroBiasX) * GYRO_SCALE_DPS;

    sample.gyDps =
        (sample.gyRaw - gyroBiasY) * GYRO_SCALE_DPS;

    sample.gzDps =
        (sample.gzRaw - gyroBiasZ) * GYRO_SCALE_DPS;


    // --------------------------------------------------------
    // TEMPERATURE CONVERSION
    // --------------------------------------------------------

    sample.temperatureC =
        (sample.temperatureRaw / 340.0f) + 36.53f;


    // --------------------------------------------------------
    // ACCELEROMETER PITCH AND ROLL
    // --------------------------------------------------------

    const float accelRoll =
        atan2f(
            sample.ayG,
            sample.azG
        ) * RAD_TO_DEG_F;

    const float accelPitch =
        atan2f(
            -sample.axG,
            sqrtf(
                sample.ayG * sample.ayG +
                sample.azG * sample.azG
            )
        ) * RAD_TO_DEG_F;


    // --------------------------------------------------------
    // COMPLEMENTARY FILTER
    // --------------------------------------------------------

    const float dt =
        (now - lastSampleMs) / 1000.0f;

    if (!filterInitialized ||
        dt <= 0.0f ||
        dt > 0.25f) {

        sample.rollDeg = accelRoll;
        sample.pitchDeg = accelPitch;

        filterInitialized = true;
    }
    else {

        // Gyroscope provides responsive movement tracking.
        // Accelerometer corrects long-term pitch/roll drift
        // when gravity is the main acceleration.

        sample.rollDeg =
            0.98f *
            (latest.rollDeg + sample.gxDps * dt) +
            0.02f * accelRoll;

        sample.pitchDeg =
            0.98f *
            (latest.pitchDeg + sample.gyDps * dt) +
            0.02f * accelPitch;
    }


    // --------------------------------------------------------
    // UPDATE LATEST SENSOR DATA
    // --------------------------------------------------------

    sample.timestampMs = now;

    sample.valid = true;

    latest = sample;

    lastSampleMs = now;
}


// ------------------------------------------------------------
// CHECK SENSOR INITIALIZATION
// ------------------------------------------------------------

bool isReady()
{
    return ready;
}


// ------------------------------------------------------------
// GET DETECTED I2C ADDRESS
// ------------------------------------------------------------

uint8_t address()
{
    return deviceAddress;
}


// ------------------------------------------------------------
// GET LATEST SENSOR DATA
// ------------------------------------------------------------

const Data &getData()
{
    return latest;
}


} // namespace MPU6050