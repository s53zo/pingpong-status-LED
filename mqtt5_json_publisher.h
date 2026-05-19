#ifndef MQTT5_JSON_PUBLISHER_H
#define MQTT5_JSON_PUBLISHER_H

#include <Arduino.h>

bool mqtt5PublishJson(const char* host,
                      uint16_t port,
                      const char* clientId,
                      const char* topic,
                      const char* payload,
                      char* error,
                      size_t errorLen);

#endif  // MQTT5_JSON_PUBLISHER_H
