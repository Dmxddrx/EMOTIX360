#include <Arduino.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "general.h"

void setup()
{
    Serial.begin(115200);

    delay(500);

    if (!General::begin()) {
        Serial.println("EMOTIX360 initialization failed!");
    }
}

void loop()
{
    // No application processing here.
    // All modules run through their own FreeRTOS tasks.

    vTaskDelay(portMAX_DELAY);
}