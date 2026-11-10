#include "max7219.h"

#include "pin_config.h"
#include "device_config.h"

#include <SPI.h>
#include <string.h>

namespace {
namespace Register {

// MAX7219 registers.
constexpr uint8_t NOOP        = 0x00;
constexpr uint8_t DIGIT0      = 0x01;
constexpr uint8_t DECODEMODE  = 0x09;
constexpr uint8_t INTENSITY   = 0x0A;
constexpr uint8_t SCANLIMIT   = 0x0B;
constexpr uint8_t SHUTDOWN    = 0x0C;
constexpr uint8_t DISPLAYTEST = 0x0F;

}

constexpr uint8_t MATRIX_WIDTH = MatrixConfig::MODULE_COUNT * 8;
constexpr uint8_t MATRIX_HEIGHT = 8;

constexpr uint8_t MAX_BRIGHTNESS = 15;

const SPISettings matrixSPI(
    MatrixConfig::SPI_CLOCK_HZ,
    MSBFIRST,
    SPI_MODE0
);

// Logical framebuffer:
// frameBuffer[0] = nearer/master module.
// frameBuffer[1] = farther/slave module.
uint8_t frameBuffer[MatrixConfig::MODULE_COUNT][8] = {};

uint8_t brightness = MatrixConfig::DEFAULT_BRIGHTNESS;

bool initialized = false;


// ------------------------------------------------------------
// SPI REGISTER WRITE
// ------------------------------------------------------------

inline void writePair(uint8_t reg, uint8_t value)
{
    SPI.transfer(reg);
    SPI.transfer(value);
}


// ------------------------------------------------------------
// WRITE SAME REGISTER TO ALL MODULES
// ------------------------------------------------------------

void writeBroadcast(uint8_t reg, uint8_t value)
{
    SPI.beginTransaction(matrixSPI);

    digitalWrite(PinConfig::MATRIX_CS, LOW);

    for (uint8_t i = 0; i < MatrixConfig::MODULE_COUNT; ++i) {
        writePair(reg, value);
    }

    digitalWrite(PinConfig::MATRIX_CS, HIGH);

    SPI.endTransaction();
}


// ------------------------------------------------------------
// SEND FRAMEBUFFER TO BOTH MODULES
// ------------------------------------------------------------

void writeFrameBuffer()
{
    SPI.beginTransaction(matrixSPI);

    for (uint8_t row = 0; row < MATRIX_HEIGHT; ++row) {

        digitalWrite(PinConfig::MATRIX_CS, LOW);

        // Send farther/slave module first,
        // then nearer/master module.
        for (
            int8_t module = MatrixConfig::MODULE_COUNT - 1;
            module >= 0;
            --module
        ) {

            writePair(
                Register::DIGIT0 + row,
                frameBuffer[module][row]
            );
        }

        digitalWrite(PinConfig::MATRIX_CS, HIGH);
    }

    SPI.endTransaction();
}

} // namespace


// ============================================================
// PUBLIC DRIVER FUNCTIONS
// ============================================================


// ------------------------------------------------------------
// INITIALIZE MAX7219 MODULES
// ------------------------------------------------------------

bool MAX7219_Init()
{
    if (initialized) {
        return true;
    }

    pinMode(PinConfig::MATRIX_CS, OUTPUT);

    digitalWrite(PinConfig::MATRIX_CS, HIGH);

    // Initialize SPI using custom ESP32-S3 pins.
    // MAX7219 does not require MISO.
    SPI.begin(
        PinConfig::MATRIX_CLK,
        -1,
        PinConfig::MATRIX_MOSI,
        PinConfig::MATRIX_CS
    );

    // Put all modules into shutdown during configuration.
    writeBroadcast(Register::SHUTDOWN, 0x00);

    writeBroadcast(Register::DISPLAYTEST, 0x00);

    // No character decoding.
    writeBroadcast(Register::DECODEMODE, 0x00);

    // Scan all eight rows.
    writeBroadcast(Register::SCANLIMIT, 0x07);

    // Configure LED intensity.
    writeBroadcast(Register::INTENSITY, brightness);

    // Clear all framebuffer data.
    memset(frameBuffer, 0, sizeof(frameBuffer));

    // Send the initial empty frame.
    writeFrameBuffer();

    // Enable normal operation.
    writeBroadcast(Register::SHUTDOWN, 0x01);

    initialized = true;

    return true;
}


// ------------------------------------------------------------
// DRIVER STATUS
// ------------------------------------------------------------

bool MAX7219_IsReady()
{
    return initialized;
}


// ------------------------------------------------------------
// SET MATRIX BRIGHTNESS
// ------------------------------------------------------------

void MAX7219_SetBrightness(uint8_t value)
{
    if (value > MAX_BRIGHTNESS) {
        value = MAX_BRIGHTNESS;
    }

    brightness = value;

    if (initialized) {
        writeBroadcast(Register::INTENSITY, brightness);
    }
}


// ------------------------------------------------------------
// GET CURRENT BRIGHTNESS
// ------------------------------------------------------------

uint8_t MAX7219_GetBrightness()
{
    return brightness;
}


// ------------------------------------------------------------
// CLEAR FRAMEBUFFER
// ------------------------------------------------------------

void MAX7219_Clear()
{
    memset(frameBuffer, 0, sizeof(frameBuffer));
}


// ------------------------------------------------------------
// SET LOGICAL FRAMEBUFFER PIXEL
// ------------------------------------------------------------

void MAX7219_SetPixel(
    int16_t x,
    int16_t y,
    uint8_t state
)
{
    if (x < 0 || x >= MATRIX_WIDTH ||
        y < 0 || y >= MATRIX_HEIGHT) {
        return;
    }

    // Preserve the original STM32 driver mapping:
    //
    // X = 0..7  : farther/slave module.
    // X = 8..15 : nearer/master module.

    const uint8_t module =
        (MatrixConfig::MODULE_COUNT - 1U) -
        (uint8_t)(x / 8);

    const uint8_t bitPos =
        7U - (uint8_t)(x % 8);

    if (state) {

        frameBuffer[module][y] |=
            (uint8_t)(1U << bitPos);
    }
    else {

        frameBuffer[module][y] &=
            (uint8_t)~(1U << bitPos);
    }
}


// ------------------------------------------------------------
// UPDATE PHYSICAL LED MATRICES
// ------------------------------------------------------------

void MAX7219_Update()
{
    if (!initialized) {
        return;
    }

    writeFrameBuffer();
}


// ------------------------------------------------------------
// DRAW CHARACTER
// ------------------------------------------------------------

void MAX7219_DrawChar(
    char ch,
    int16_t x,
    int16_t y,
    const FontDef_t *font
)
{
    if (font == nullptr || font->data == nullptr ||
        ch < 32 || ch > 126) {
        return;
    }

    const uint16_t offset =
        (uint16_t)(ch - 32) * font->FontHeight;

    for (uint8_t row = 0; row < font->FontHeight; ++row) {

        const uint16_t rowBits =
            font->data[offset + row];

        for (
            uint8_t col = 0;
            col < font->FontWidth && col < 16;
            ++col
        ) {

            if (rowBits & (uint16_t)(0x8000U >> col)) {

                MAX7219_SetPixel(
                    x + col,
                    y + row,
                    1
                );
            }
        }
    }
}


// ------------------------------------------------------------
// DRAW TEXT STRING
// ------------------------------------------------------------

void MAX7219_DrawString(
    const char *str,
    int16_t x,
    int16_t y,
    const FontDef_t *font
)
{
    if (str == nullptr || font == nullptr) {
        return;
    }

    while (*str) {

        MAX7219_DrawChar(
            *str++,
            x,
            y,
            font
        );

        x += font->FontWidth;
    }
}


// ------------------------------------------------------------
// DRAW ICON
// ------------------------------------------------------------

void MAX7219_DrawIcon(
    const IconDef_t *icon,
    int16_t x,
    int16_t y
)
{
    if (icon == nullptr || icon->data == nullptr) {
        return;
    }

    for (uint8_t row = 0; row < icon->height; ++row) {

        const uint8_t rowBits = icon->data[row];

        for (
            uint8_t col = 0;
            col < icon->width && col < 8;
            ++col
        ) {

            if (rowBits & (uint8_t)(0x80U >> col)) {

                MAX7219_SetPixel(
                    x + col,
                    y + row,
                    1
                );
            }
        }
    }
}