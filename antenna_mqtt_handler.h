#ifndef ANTENNA_MQTT_HANDLER_H
#define ANTENNA_MQTT_HANDLER_H

#include <ArduinoJson.h>

// ❶  extern tells the compiler “this variable lives elsewhere”
extern DynamicJsonDocument availableDoc;

// Function to parse antenna availability JSON
void handleAvailableJSON(const char* json);

// Function to parse current band JSON
void handleCurrentBandJSON(const char* json);

#endif
