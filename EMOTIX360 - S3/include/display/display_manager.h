#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Arduino.h>
#include <time.h>

namespace DisplayManager {

struct Status {

    bool timeValid = false;
    struct tm localTime = {};

    bool mpuReady = false;
    float pitchDeg = 0.0f;
    float rollDeg = 0.0f;

    bool wifiConnected = false;
    bool ntpSynced = false;

    char ipAddress[16] = "-";
    char modeName[24] = "EMOTIX360";

    uint8_t page = 0;
    uint8_t brightness = 5;

    bool externalPowerPresent = false;
    int batteryPercent = -1;
};

bool begin();
bool startTask();
bool isReady();

void setNetworkStatus(
    bool wifiConnected,
    bool ntpSynced,
    const char *ipAddress
);

void setPowerStatus(
    bool externalPower,
    int batteryPercent
);

void setClockStatus(
    const struct tm &time,
    bool valid
);

void setMPUStatus(
    bool ready,
    float pitchDeg,
    float rollDeg
);

void setApplicationStatus(
    uint8_t page,
    const char *modeName
);

void requestUpdate();

void showStartupMessage(const char *message);
void logMessage(const char *format, ...);

}

#endif