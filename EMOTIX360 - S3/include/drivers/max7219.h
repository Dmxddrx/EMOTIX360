#ifndef MAX7219_H
#define MAX7219_H

#include <Arduino.h>

#include "fonts.h"
#include "icons.h"

bool MAX7219_Init();
bool MAX7219_IsReady();

void MAX7219_SetBrightness(uint8_t brightness);
uint8_t MAX7219_GetBrightness();

void MAX7219_Clear();
void MAX7219_Update();

void MAX7219_SetPixel(
    int16_t x,
    int16_t y,
    uint8_t state
);

void MAX7219_DrawChar(
    char ch,
    int16_t x,
    int16_t y,
    const FontDef_t *font
);

void MAX7219_DrawString(
    const char *str,
    int16_t x,
    int16_t y,
    const FontDef_t *font
);

void MAX7219_DrawIcon(
    const IconDef_t *icon,
    int16_t x,
    int16_t y
);

#endif