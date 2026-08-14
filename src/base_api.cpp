#include "base_api.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <AqualinkD.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <stdarg.h>

#include "config.h"
#include "panel_state.h"
#include "web_ui.h"

namespace BaseApi {
namespace {

constexpr size_t RAW_LINE_COUNT = 60;
constexpr size_t RAW_LINE_LENGTH = 512;
constexpr uint32_t WIFI_RETRY_MS = 15000;
constexpr uint32_t MQTT_RETRY_MS = 5000;

AsyncWebServer server(HTTP_PORT);
WiFiClient mqttTransport;
PubSubClient mqtt(mqttTransport);
SemaphoreHandle_t rawMutex = nullptr;
char rawLines[RAW_LINE_COUNT][RAW_LINE_LENGTH] = {};
size_t rawHead = 0;
size_t rawCount = 0;
volatile uint32_t rawRevision = 0;
volatile uint32_t droppedPacketCount = 0;
bool serverStarted = false;

const char* commandName(uint8_t command) {
    switch (command) {
        case aqualinkd::CMD_PROBE:     return "PROBE";
        case aqualinkd::CMD_ACK:       return "ACK";
        case aqualinkd::CMD_STATUS:    return "STATUS";
        case aqualinkd::CMD_MSG:       return "MSG";
        case aqualinkd::CMD_MSG_LONG:  return "MSGL";
        case aqualinkd::CMD_PROBE_ALT: return "PROBE2";
        default:                       return "?";
    }
}

size_t append(char* output, size_t capacity, size_t length,
              const char* format, ...) {
    if (length >= capacity) {
        return capacity;
    }

    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(output + length, capacity - length,
                                  format, arguments);
    va_end(arguments);
    if (written < 0) {
        return length;
    }
    const size_t count = static_cast<size_t>(written);
    return count >= capacity - length ? capacity - 1 : length + count;
}

void formatPacket(const aqualinkd::Packet& packet, char* output,
                  size_t capacity) {
    size_t length = append(output, capacity, 0, "%8lu  %02X %-6s ",
                           static_cast<unsigned long>(millis()),
                           packet.destination, commandName(packet.command));
    for (size_t i = 0; i < packet.dataLength; ++i) {
        length = append(output, capacity, length, "%02X ", packet.data[i]);
    }

    if (packet.command == aqualinkd::CMD_MSG ||
        packet.command == aqualinkd::CMD_MSG_LONG) {
        length = append(output, capacity, length, " |");
        const size_t offset = packet.command == aqualinkd::CMD_MSG_LONG &&
                              packet.dataLength > 0 ? 1 : 0;
        for (size_t i = offset; i < packet.dataLength && length + 1 < capacity;
             ++i) {
            const char value = static_cast<char>(packet.data[i]);
            output[length++] = value >= 32 && value < 127 ? value : '.';
        }
        if (length + 1 < capacity) {
            output[length++] = '|';
        }
        output[length] = '\0';
    }
}

void buildStatusJson(String& body, bool includeButtons) {
    const aqualinkd::BusStats stats = aqualinkd::Bus.stats();
    JsonDocument document;
    document["online"] = aqualinkd::Bus.online();
    document["bus_running"] = aqualinkd::Bus.running();
    document["uptime_ms"] = millis();
    document["free_heap"] = ESP.getFreeHeap();
    document["packets"] = stats.packetsReceived;
    document["acks"] = stats.acknowledgementsSent;
    document["bad_checksums"] = stats.checksumErrors;
    document["overflows"] = stats.framesOverflowed;
    document["echoes_dropped"] = stats.echoesDropped;
    document["packets_dropped"] = droppedPacketCount;
    document["ack_latency_us"] = stats.acknowledgementLatencyUs;
    document["keys_queued"] = aqualinkd::Bus.pendingKeys();
    document["sniff_only"] = JANDY_SNIFF_ONLY != 0;
    document["wifi_connected"] = WiFi.status() == WL_CONNECTED;
    document["mqtt_connected"] = MQTT_ENABLED != 0 && mqtt.connected();
    if (WiFi.status() == WL_CONNECTED) {
        document["ip"] = WiFi.localIP().toString();
        document["wifi_rssi"] = WiFi.RSSI();
    }

    if (includeButtons) {
        const PanelSnapshot snapshot = Panel.snapshot();
        document["button_revision"] = snapshot.revision;
        document["display"] = snapshot.display;
        document["display_revision"] = snapshot.displayRevision;
        JsonArray buttons = document["buttons"].to<JsonArray>();
        for (size_t i = 0; i < PANEL_BUTTON_COUNT; ++i) {
            JsonObject button = buttons.add<JsonObject>();
            button["index"] = i;
            button["name"] = PanelModel::buttonName(i);
            button["state"] = PanelModel::stateName(snapshot.buttons[i]);
            button["on"] = PanelModel::isActive(snapshot.buttons[i]);
        }
    }

    serializeJson(document, body);
}

void sendStatus(AsyncWebServerRequest* request) {
    String body;
    buildStatusJson(body, false);
    request->send(200, "application/json", body);
}

void sendState(AsyncWebServerRequest* request) {
    String body;
    buildStatusJson(body, true);
    request->send(200, "application/json", body);
}

void sendConfig(AsyncWebServerRequest* request) {
    JsonDocument document;
    document["hostname"] = DEVICE_HOSTNAME;
    document["device_id"] = JANDY_MY_ID;
    document["sniff_only"] = JANDY_SNIFF_ONLY != 0;
    document["promiscuous"] = JANDY_PROMISCUOUS != 0;
    document["uart"] = static_cast<int>(JANDY_UART_NUM);
    document["baud"] = JANDY_BAUD;
    document["rx_pin"] = JANDY_PIN_RX;
    document["tx_pin"] = JANDY_PIN_TX;
    document["de_pin"] = JANDY_PIN_DE;
    document["echo_window_us"] = JANDY_ECHO_WINDOW_US;
    document["panel_button_count"] = PANEL_BUTTON_COUNT;
    document["http_port"] = HTTP_PORT;
    document["mqtt_enabled"] = MQTT_ENABLED != 0;
    document["mqtt_host"] = MQTT_HOST;
    document["mqtt_port"] = MQTT_PORT;
    document["mqtt_base_topic"] = MQTT_BASE_TOPIC;

    // Passwords and broker credentials are intentionally never returned.
    String body;
    serializeJson(document, body);
    request->send(200, "application/json", body);
}

void sendRaw(AsyncWebServerRequest* request) {
    String body;
    body.reserve(rawCount * 64);
    if (rawMutex != nullptr && xSemaphoreTake(rawMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        const size_t oldest = (rawHead + RAW_LINE_COUNT - rawCount) % RAW_LINE_COUNT;
        for (size_t i = 0; i < rawCount; ++i) {
            body += rawLines[(oldest + i) % RAW_LINE_COUNT];
            body += '\n';
        }
        xSemaphoreGive(rawMutex);
    }
    request->send(200, "text/plain; charset=utf-8", body);
}

bool parseByteParameter(AsyncWebServerRequest* request, const char* name,
                        long minimum, long maximum, uint8_t& result) {
    if (!request->hasParam(name)) {
        return false;
    }
    const String text = request->getParam(name)->value();
    char* end = nullptr;
    const long value = strtol(text.c_str(), &end, 0);
    while (end != nullptr && *end == ' ') {
        ++end;
    }
    if (end == text.c_str() || (end != nullptr && *end != '\0') ||
        value < minimum || value > maximum) {
        return false;
    }
    result = static_cast<uint8_t>(value);
    return true;
}

void queueHttpKey(AsyncWebServerRequest* request, uint8_t keyCode) {
    if (JANDY_SNIFF_ONLY != 0) {
        request->send(403, "application/json",
                      "{\"error\":\"controls disabled in sniff-only mode\"}");
        return;
    }
    if (!aqualinkd::Bus.online()) {
        request->send(409, "application/json",
                      "{\"error\":\"panel bus is offline\"}");
        return;
    }
    if (!aqualinkd::Bus.queueKey(keyCode)) {
        request->send(503, "application/json",
                      "{\"error\":\"key queue is full\"}");
        return;
    }

    char body[48];
    snprintf(body, sizeof(body), "{\"result\":\"queued\",\"key\":%u}",
             static_cast<unsigned>(keyCode));
    request->send(202, "application/json", body);
}

void sendButtonPress(AsyncWebServerRequest* request) {
    uint8_t index = 0;
    if (!parseByteParameter(request, "index", 0, PANEL_BUTTON_COUNT - 1,
                            index)) {
        request->send(400, "application/json",
                      "{\"error\":\"index must identify an RS-8 button\"}");
        return;
    }
    queueHttpKey(request, PanelModel::keyCode(index));
}

void sendRawKey(AsyncWebServerRequest* request) {
    uint8_t keyCode = 0;
    if (!parseByteParameter(request, "code", 1, 0xFF, keyCode)) {
        request->send(400, "application/json",
                      "{\"error\":\"code must be a byte from 1 to 255\"}");
        return;
    }
    queueHttpKey(request, keyCode);
}

void configureRoutes() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "text/html; charset=utf-8", WEB_UI);
    });
    server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(204);
    });
    server.on("/api/status", HTTP_GET, sendStatus);
    server.on("/api/state", HTTP_GET, sendState);
    server.on("/api/config", HTTP_GET, sendConfig);
    server.on("/api/raw", HTTP_GET, sendRaw);
    server.on("/api/button", HTTP_POST, sendButtonPress);
    server.on("/api/key", HTTP_POST, sendRawKey);
    server.onNotFound([](AsyncWebServerRequest* request) {
        request->send(404, "application/json", "{\"error\":\"not found\"}");
    });
}

String mqttTopic(const char* suffix) {
    return String(MQTT_BASE_TOPIC) + "/" + suffix;
}

void publishMqttResult(const char* result) {
    const String topic = mqttTopic("key/result");
    mqtt.publish(topic.c_str(), result, false);
}

void onMqttMessage(char* topic, uint8_t* payload, unsigned int length) {
    const String keyTopic = mqttTopic("key/set");
    if (keyTopic != topic) {
        return;
    }
    if (JANDY_SNIFF_ONLY != 0) {
        publishMqttResult("sniff_only");
        return;
    }
    if (length == 0 || length >= 16) {
        publishMqttResult("invalid_key");
        return;
    }

    char value[16] = {};
    memcpy(value, payload, length);
    char* end = nullptr;
    const long keyCode = strtol(value, &end, 0);
    while (end != nullptr && *end == ' ') {
        ++end;
    }
    if (end == value || (end != nullptr && *end != '\0') ||
        keyCode <= 0 || keyCode > 0xFF) {
        publishMqttResult("invalid_key");
        return;
    }

    publishMqttResult(aqualinkd::Bus.queueKey(static_cast<uint8_t>(keyCode))
                          ? "queued"
                          : "queue_full");
}

bool publishMqttState() {
    String body;
    buildStatusJson(body, true);
    const String topic = mqttTopic("state");
    return mqtt.publish(topic.c_str(), body.c_str(), true);
}

void publishButtonChanges(PanelSnapshot& published, bool force) {
    const PanelSnapshot current = Panel.snapshot();
    if (!force && current.revision == published.revision) {
        return;
    }

    bool allPublished = true;
    for (size_t i = 0; i < PANEL_BUTTON_COUNT; ++i) {
        if (!force && current.buttons[i] == published.buttons[i]) {
            continue;
        }
        const String topic = String(MQTT_BASE_TOPIC) + "/button/" +
                             PanelModel::buttonName(i) + "/state";
        allPublished &= mqtt.publish(
            topic.c_str(), PanelModel::stateName(current.buttons[i]), true);
    }

    allPublished &= publishMqttState();
    if (allPublished && mqtt.connected()) {
        published = current;
    }
}

void publishLatestRaw(uint32_t& publishedRevision) {
    if (rawMutex == nullptr || rawRevision == publishedRevision) {
        return;
    }

    char line[RAW_LINE_LENGTH] = {};
    uint32_t revision = publishedRevision;
    if (xSemaphoreTake(rawMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (rawCount > 0) {
            const size_t newest = (rawHead + RAW_LINE_COUNT - 1) % RAW_LINE_COUNT;
            strlcpy(line, rawLines[newest], sizeof(line));
            revision = rawRevision;
        }
        xSemaphoreGive(rawMutex);
    }
    if (line[0] == '\0') {
        return;
    }

    const String topic = mqttTopic("raw");
    if (mqtt.publish(topic.c_str(), line, false)) {
        publishedRevision = revision;
    }
}

bool connectMqtt() {
    const String availabilityTopic = mqttTopic("status");
    const bool connected = strlen(MQTT_USER) > 0
        ? mqtt.connect(DEVICE_HOSTNAME, MQTT_USER, MQTT_PASS,
                       availabilityTopic.c_str(), 0, true, "offline")
        : mqtt.connect(DEVICE_HOSTNAME, nullptr, nullptr,
                       availabilityTopic.c_str(), 0, true, "offline");
    if (!connected) {
        return false;
    }

    mqtt.publish(availabilityTopic.c_str(), "online", true);
    const String keyTopic = mqttTopic("key/set");
    mqtt.subscribe(keyTopic.c_str());
    Serial.printf("[mqtt] connected broker=%s:%u client=%s base_topic=%s\n",
                  MQTT_HOST, static_cast<unsigned>(MQTT_PORT),
                  DEVICE_HOSTNAME, MQTT_BASE_TOPIC);
    return true;
}

void apiTask(void*) {
    Serial.printf("[net] hostname=%s ssid=%s ipv4=DHCP http_port=%u\n",
                  DEVICE_HOSTNAME, WIFI_SSID,
                  static_cast<unsigned>(HTTP_PORT));
    if (MQTT_ENABLED != 0) {
        Serial.printf("[mqtt] enabled broker=%s:%u base_topic=%s auth=%s\n",
                      MQTT_HOST, static_cast<unsigned>(MQTT_PORT),
                      MQTT_BASE_TOPIC,
                      strlen(MQTT_USER) > 0 ? "configured" : "anonymous");
    } else {
        Serial.println("[mqtt] disabled");
    }

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(DEVICE_HOSTNAME);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    configureRoutes();
    server.begin();
    serverStarted = true;
    Serial.printf("[api] HTTP server started on port %u\n",
                  static_cast<unsigned>(HTTP_PORT));

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMqttMessage);
    mqtt.setBufferSize(1536);

    wl_status_t previousWifiStatus = WL_IDLE_STATUS;
    bool previousMqttStatus = false;
    uint32_t lastWifiRetry = millis();
    uint32_t lastMqttRetry = 0;
    uint32_t publishedRawRevision = 0;
    PanelSnapshot publishedPanel = {};
    for (;;) {
        const wl_status_t wifiStatus = WiFi.status();
        if (wifiStatus == WL_CONNECTED && previousWifiStatus != WL_CONNECTED) {
            Serial.printf("[net] WiFi connected ip=%s gateway=%s subnet=%s "
                          "dns=%s rssi=%d dBm\n",
                          WiFi.localIP().toString().c_str(),
                          WiFi.gatewayIP().toString().c_str(),
                          WiFi.subnetMask().toString().c_str(),
                          WiFi.dnsIP().toString().c_str(), WiFi.RSSI());
            Serial.printf("[api] web UI: http://%s:%u/\n",
                          WiFi.localIP().toString().c_str(),
                          static_cast<unsigned>(HTTP_PORT));
        } else if (wifiStatus != WL_CONNECTED &&
                   previousWifiStatus == WL_CONNECTED) {
            Serial.println("[api] WiFi disconnected");
        }
        previousWifiStatus = wifiStatus;

        if (wifiStatus != WL_CONNECTED &&
            millis() - lastWifiRetry >= WIFI_RETRY_MS) {
            lastWifiRetry = millis();
            WiFi.reconnect();
        }

        if (MQTT_ENABLED != 0 && wifiStatus == WL_CONNECTED) {
            if (!mqtt.connected() && millis() - lastMqttRetry >= MQTT_RETRY_MS) {
                lastMqttRetry = millis();
                if (connectMqtt()) {
                    publishButtonChanges(publishedPanel, true);
                }
            }
            if (mqtt.connected()) {
                mqtt.loop();
                publishLatestRaw(publishedRawRevision);
                publishButtonChanges(publishedPanel, false);
            }
        }

        const bool mqttStatus = mqtt.connected();
        if (!mqttStatus && previousMqttStatus) {
            Serial.println("[mqtt] disconnected");
        }
        previousMqttStatus = mqttStatus;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

}  // namespace

bool start() {
    if (serverStarted || rawMutex != nullptr) {
        return false;
    }
    rawMutex = xSemaphoreCreateMutex();
    if (rawMutex == nullptr) {
        return false;
    }

    if (xTaskCreatePinnedToCore(apiTask, "base-api", 6144, nullptr, 2,
                                nullptr, 0) != pdPASS) {
        vSemaphoreDelete(rawMutex);
        rawMutex = nullptr;
        return false;
    }
    return true;
}

void recordPacket(const aqualinkd::Packet& packet) {
    if (rawMutex == nullptr) {
        return;
    }

    char line[RAW_LINE_LENGTH] = {};
    formatPacket(packet, line, sizeof(line));
    if (xSemaphoreTake(rawMutex, portMAX_DELAY) == pdTRUE) {
        strlcpy(rawLines[rawHead], line, RAW_LINE_LENGTH);
        rawHead = (rawHead + 1) % RAW_LINE_COUNT;
        if (rawCount < RAW_LINE_COUNT) {
            ++rawCount;
        }
        ++rawRevision;
        xSemaphoreGive(rawMutex);
    }
}

void noteDroppedPacket() {
    ++droppedPacketCount;
}

uint32_t droppedPackets() {
    return droppedPacketCount;
}

}  // namespace BaseApi
