#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <time.h>

namespace WifiManager {

bool begin();
bool startTask();

bool isTaskRunning();
bool isConnected();

bool isTimeValid();
bool isTimeSynced();

bool getLocalTime(struct tm &out);

bool getIPAddress(char *buffer, size_t bufferSize);
bool getSSID(char *buffer, size_t bufferSize);

}

#endif