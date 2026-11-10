
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

void setup()
{
    Serial.begin(115200);
    delay(1500);

    Serial.println();
    Serial.println("================================");
    Serial.println("       EMOTIX360 BOOT TEST");
    Serial.println("================================");

    Serial.printf("Chip model: %s\n", ESP.getChipModel());
    Serial.printf("Chip revision: %u\n", ESP.getChipRevision());
    Serial.printf("CPU frequency: %u MHz\n", ESP.getCpuFreqMHz());

    Serial.printf("Flash size: %u MB\n",
                  ESP.getFlashChipSize() / (1024 * 1024));

    Serial.printf("PSRAM detected: %s\n",
                  psramFound() ? "YES" : "NO");

    Serial.printf("PSRAM size: %u MB\n",
                  ESP.getPsramSize() / (1024 * 1024));

    Serial.printf("Free PSRAM: %u bytes\n",
                  ESP.getFreePsram());

    Serial.printf("Free internal heap: %u bytes\n",
                  ESP.getFreeHeap());

    // Test an allocation from external PSRAM.
    void *psramTest = heap_caps_malloc(
        1024 * 1024,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (psramTest != nullptr)
    {
        Serial.println("PSRAM allocation test: PASS");

        memset(psramTest, 0xAA, 1024 * 1024);

        Serial.println("PSRAM write test: PASS");

        heap_caps_free(psramTest);
    }
    else
    {
        Serial.println("PSRAM allocation test: FAILED");
    }

    Serial.println("================================");
    Serial.println("       BOOT TEST COMPLETE");
    Serial.println("================================");
}

void loop()
{
    delay(1000);
}
