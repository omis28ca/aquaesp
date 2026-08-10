#pragma once
#include <Arduino.h>

namespace NetApi {

void begin();          // WiFi + HTTP + WebSocket + MQTT, from setup()
void loop();           // pump MQTT and push changes, from the core-0 task

// Append a line to the rolling raw-packet log served at /api/raw.
void logRaw(const char *line);

}  // namespace NetApi
