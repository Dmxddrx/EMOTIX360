#include <Arduino.h>

#include "general.h"

void setup()
{
    General::begin();
}

void loop()
{
    // All application tasks run independently.
    // No application polling or processing here.

    vTaskDelay(portMAX_DELAY);
}