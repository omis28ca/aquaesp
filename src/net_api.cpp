#include "net_api.h"
#include "config.h"
#include "panel_state.h"
#include "jandy_serial.h"
#include "keycodes.h"

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

namespace NetApi {

static AsyncWebServer server(HTTP_PORT);
static AsyncWebSocket ws("/ws");

static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);

static uint32_t lastPushedRevision = 0;
static uint32_t lastMqttAttempt = 0;
static bool     discoveryPublished = false;

// --- rolling raw log --------------------------------------------------------
static const size_t RAW_LOG_LINES = 60;
static String rawLog[RAW_LOG_LINES];
static size_t rawLogHead = 0;
static portMUX_TYPE rawMux = portMUX_INITIALIZER_UNLOCKED;

void logRaw(const char *line) {
    portENTER_CRITICAL(&rawMux);
    rawLog[rawLogHead] = line;
    rawLogHead = (rawLogHead + 1) % RAW_LOG_LINES;
    portEXIT_CRITICAL(&rawMux);
}

// ---------------------------------------------------------------------------
// State serialisation
// ---------------------------------------------------------------------------
static size_t buttonCount() {
    size_t n = PANEL_BUTTON_COUNT;
    return n < BUTTON_TABLE_LEN ? n : BUTTON_TABLE_LEN;
}

static void buildStateJson(String &out) {
    PanelState s = Panel.snapshot();

    JsonDocument doc;
    doc["online"]      = Bus.online();
    doc["message"]     = s.message;
    doc["panel"]       = s.panelModel;
    doc["service"]     = s.serviceMode;
    doc["packets"]     = Bus.packetsSeen();
    doc["bad_cksum"]   = Bus.badChecksums();
    doc["resyncs"]     = Bus.resyncs();
    // Watch this. Above ~20000 the panel starts dropping us; it means
    // something has begun stealing time on core 1.
    doc["ack_latency_us"] = Bus.ackLatencyUs();
    doc["echoes_dropped"] = Bus.echoesDropped();
    doc["keys_queued"] = (uint32_t)Bus.pendingKeys();

    if (s.airTempF  > -999) doc["air_temp"]  = s.airTempF;
    if (s.poolTempF > -999) doc["pool_temp"] = s.poolTempF;
    if (s.spaTempF  > -999) doc["spa_temp"]  = s.spaTempF;
    if (s.poolSetpointF > -999) doc["pool_setpoint"] = s.poolSetpointF;
    if (s.spaSetpointF  > -999) doc["spa_setpoint"]  = s.spaSetpointF;

    JsonArray btns = doc["buttons"].to<JsonArray>();
    for (size_t i = 0; i < buttonCount(); i++) {
        JsonObject b = btns.add<JsonObject>();
        b["index"] = (int)i;
        b["name"]  = BUTTON_NAMES[i];
        b["state"] = ledStateName(s.led[i]);
        b["on"]    = (s.led[i] == LED_ON);
    }

    serializeJson(doc, out);
}

// ---------------------------------------------------------------------------
// Command handling -- shared by REST and MQTT
// ---------------------------------------------------------------------------
// There is no "set circuit to X" command on this bus. A keypress toggles, so
// an idempotent on/off has to compare against the current LED state first.
static bool applyButtonCommand(size_t idx, const String &verb, String &err) {
    if (idx >= buttonCount()) { err = "button index out of range"; return false; }

    PanelState s = Panel.snapshot();
    bool isOn = (s.led[idx] == LED_ON);

    bool wantPress;
    if (verb == "toggle")   wantPress = true;
    else if (verb == "on")  wantPress = !isOn;
    else if (verb == "off") wantPress = isOn;
    else { err = "verb must be on, off or toggle"; return false; }

    if (!wantPress) return true;    // already in the requested state
    if (!Bus.pressKey(BUTTON_KEYS[idx])) { err = "key queue full"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
static void setupHttp() {
    server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest *req) {
        String body; buildStateJson(body);
        req->send(200, "application/json", body);
    });

    // POST /api/button/3/on
    server.on("^\\/api\\/button\\/([0-9]+)\\/(on|off|toggle)$", HTTP_POST,
              [](AsyncWebServerRequest *req) {
        size_t idx = req->pathArg(0).toInt();
        String verb = req->pathArg(1);
        String err;
        if (applyButtonCommand(idx, verb, err))
            req->send(200, "application/json", "{\"ok\":true}");
        else
            req->send(400, "application/json", "{\"ok\":false,\"error\":\"" + err + "\"}");
    });

    // POST /api/key/0x05 -- raw keypress, for menu walking and for testing
    // codes you are learning from the wall keypad.
    server.on("^\\/api\\/key\\/([0-9a-fA-Fx]+)$", HTTP_POST,
              [](AsyncWebServerRequest *req) {
        uint8_t code = (uint8_t)strtol(req->pathArg(0).c_str(), NULL, 0);
        bool ok = Bus.pressKey(code);
        req->send(ok ? 200 : 503, "application/json",
                  ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"queue full\"}");
    });

    server.on("/api/raw", HTTP_GET, [](AsyncWebServerRequest *req) {
        String body;
        body.reserve(RAW_LOG_LINES * 48);
        portENTER_CRITICAL(&rawMux);
        for (size_t i = 0; i < RAW_LOG_LINES; i++) {
            const String &l = rawLog[(rawLogHead + i) % RAW_LOG_LINES];
            if (l.length()) { body += l; body += '\n'; }
        }
        portEXIT_CRITICAL(&rawMux);
        req->send(200, "text/plain", body);
    });

    ws.onEvent([](AsyncWebSocket *s, AsyncWebSocketClient *c,
                  AwsEventType type, void *arg, uint8_t *data, size_t len) {
        if (type == WS_EVT_CONNECT) {
            String body; buildStateJson(body);
            c->text(body);
        }
    });
    server.addHandler(&ws);

    server.begin();
}

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------
static String topicBase() { return String(MQTT_BASE_TOPIC); }

static void publishDiscovery() {
    String uid = String(DEVICE_HOSTNAME);

    for (size_t i = 0; i < buttonCount(); i++) {
        JsonDocument doc;
        doc["name"]         = BUTTON_NAMES[i];
        doc["unique_id"]    = uid + "_" + BUTTON_NAMES[i];
        doc["state_topic"]  = topicBase() + "/button/" + BUTTON_NAMES[i] + "/state";
        doc["command_topic"]= topicBase() + "/button/" + BUTTON_NAMES[i] + "/set";
        doc["payload_on"]   = "on";
        doc["payload_off"]  = "off";
        doc["availability_topic"] = topicBase() + "/status";

        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"] = uid;
        dev["name"]        = "Jandy Aqualink RS";
        dev["manufacturer"]= "Jandy";
        dev["model"]       = "Aqualink RS (All Button bridge)";

        String payload; serializeJson(doc, payload);
        String t = String(MQTT_HA_DISCOVERY_PREFIX) + "/switch/" + uid + "_" +
                   BUTTON_NAMES[i] + "/config";
        mqtt.publish(t.c_str(), payload.c_str(), true);
    }

    struct { const char *key; const char *name; const char *cls; } sensors[] = {
        {"air_temp",  "Air Temperature",  "temperature"},
        {"pool_temp", "Pool Temperature", "temperature"},
        {"spa_temp",  "Spa Temperature",  "temperature"},
    };
    for (auto &s : sensors) {
        JsonDocument doc;
        doc["name"]              = s.name;
        doc["unique_id"]         = uid + "_" + s.key;
        doc["state_topic"]       = topicBase() + "/state";
        doc["value_template"]    = String("{{ value_json.") + s.key + " }}";
        doc["device_class"]      = s.cls;
        doc["unit_of_measurement"] = "°F";
        doc["availability_topic"]= topicBase() + "/status";
        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"] = uid;
        dev["name"]        = "Jandy Aqualink RS";

        String payload; serializeJson(doc, payload);
        String t = String(MQTT_HA_DISCOVERY_PREFIX) + "/sensor/" + uid + "_" +
                   s.key + "/config";
        mqtt.publish(t.c_str(), payload.c_str(), true);
    }
}

static void onMqttMessage(char *topic, uint8_t *payload, unsigned int len) {
    String t(topic);
    String v; v.reserve(len);
    for (unsigned int i = 0; i < len; i++) v += (char)payload[i];
    v.toLowerCase();

    String prefix = topicBase() + "/button/";
    if (!t.startsWith(prefix)) return;
    String rest = t.substring(prefix.length());
    int slash = rest.indexOf('/');
    if (slash < 0) return;
    String name = rest.substring(0, slash);

    for (size_t i = 0; i < buttonCount(); i++) {
        if (name == BUTTON_NAMES[i]) {
            String err;
            applyButtonCommand(i, v, err);
            return;
        }
    }
}

static void mqttReconnect() {
    if (millis() - lastMqttAttempt < 5000) return;
    lastMqttAttempt = millis();

    String willTopic = topicBase() + "/status";
    bool ok = strlen(MQTT_USER)
        ? mqtt.connect(DEVICE_HOSTNAME, MQTT_USER, MQTT_PASS,
                       willTopic.c_str(), 0, true, "offline")
        : mqtt.connect(DEVICE_HOSTNAME, NULL, NULL,
                       willTopic.c_str(), 0, true, "offline");
    if (!ok) return;

    mqtt.publish(willTopic.c_str(), "online", true);
    mqtt.subscribe((topicBase() + "/button/+/set").c_str());
    if (!discoveryPublished) { publishDiscovery(); discoveryPublished = true; }
    lastPushedRevision = 0;   // force a full publish
}

// ---------------------------------------------------------------------------
static void publishState() {
    String body; buildStateJson(body);

    ws.textAll(body);

    if (MQTT_ENABLED && mqtt.connected()) {
        mqtt.publish((topicBase() + "/state").c_str(), body.c_str(), true);
        PanelState s = Panel.snapshot();
        for (size_t i = 0; i < buttonCount(); i++) {
            String t = topicBase() + "/button/" + BUTTON_NAMES[i] + "/state";
            mqtt.publish(t.c_str(), s.led[i] == LED_ON ? "on" : "off", true);
        }
    }
}

// ---------------------------------------------------------------------------
void begin() {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(DEVICE_HOSTNAME);
    WiFi.setSleep(false);            // latency matters more than milliamps here
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
    Serial.printf("[net] wifi %s  ip=%s\n",
                  WiFi.status() == WL_CONNECTED ? "up" : "FAILED",
                  WiFi.localIP().toString().c_str());

    setupHttp();

    if (MQTT_ENABLED) {
        mqtt.setServer(MQTT_HOST, MQTT_PORT);
        mqtt.setCallback(onMqttMessage);
        mqtt.setBufferSize(1536);    // discovery payloads exceed the default 256
    }
}

void loop() {
    if (MQTT_ENABLED) {
        if (!mqtt.connected()) mqttReconnect();
        else mqtt.loop();
    }

    uint32_t rev = Panel.revision();
    if (rev != lastPushedRevision) {
        lastPushedRevision = rev;
        publishState();
    }

    ws.cleanupClients();
}

}  // namespace NetApi
