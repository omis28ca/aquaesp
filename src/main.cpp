#include <Arduino.h>
#include "config.h"
#include "jandy_serial.h"
#include "panel_state.h"
#include "net_api.h"

// ---------------------------------------------------------------------------
// Core assignment
// ---------------------------------------------------------------------------
//   core 1 : RS-485 task (created inside JandyBus::begin), high priority.
//            Blocks on uart_read_bytes, wakes on a frame, ACKs, parses.
//   core 0 : this netTask, plus the WiFi driver and AsyncTCP.
//
// Arduino's loopTask also lives on core 1 but at priority 1, so it can never
// hold off the RS-485 task. We leave it idle.

static const char *cmdName(uint8_t cmd) {
    switch (cmd) {
    case CMD_PROBE:      return "PROBE";
    case CMD_ACK:        return "ACK";
    case CMD_STATUS:     return "STATUS";
    case CMD_MSG:        return "MSG";
    case CMD_MSG_LONG:   return "MSGL";
    case CMD_PROBE_ALT:  return "PROBE2";
    default:             return "?";
    }
}

// Every packet on the bus, including traffic between the panel and devices we
// are not impersonating. This is how the learn mode below works, and it is
// what you want on screen during bring-up.
static void onSnoop(const JandyPacket &p) {
    char line[160];
    int n = snprintf(line, sizeof(line), "%8lu  %02X %-6s ",
                     (unsigned long)millis(), p.dest, cmdName(p.cmd));
    for (uint8_t i = 0; i < p.len && n < (int)sizeof(line) - 4; i++)
        n += snprintf(line + n, sizeof(line) - n, "%02X ", p.data[i]);

    // Render the payload as text too -- MSG packets are plain ASCII and it is
    // far quicker to read the display line than to decode hex by eye.
    if (p.cmd == CMD_MSG || p.cmd == CMD_MSG_LONG) {
        n += snprintf(line + n, sizeof(line) - n, " |");
        for (uint8_t i = 0; i < p.len && n < (int)sizeof(line) - 3; i++) {
            char c = (char)p.data[i];
            line[n++] = (c >= 32 && c < 127) ? c : '.';
        }
        line[n++] = '|';
        line[n] = 0;
    }

    Serial.println(line);
    NetApi::logRaw(line);

    // --- Learn mode ---------------------------------------------------------
    // Replies from OTHER keypads are addressed to the master with CMD_ACK.
    // data[1] is the keycode. Press a button on your wall keypad and the code
    // it sends shows up here -- that is the value to put in keycodes.h.
    if (p.dest == DEV_MASTER && p.cmd == CMD_ACK && p.len >= 2 && p.data[1] != 0) {
        Serial.printf("LEARN: a keypad on this bus sent key 0x%02X\n", p.data[1]);
    }
}

static void netTask(void *) {
    NetApi::begin();
    for (;;) {
        NetApi::loop();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.printf("Jandy Aqualink RS bridge -- impersonating keypad 0x%02X%s\n",
                  JANDY_MY_ID, JANDY_SNIFF_ONLY ? " (SNIFF ONLY, not transmitting)" : "");

    Bus.onPacket([](const JandyPacket &p) { Panel.handlePacket(p); });
#if JANDY_PROMISCUOUS
    Bus.onSnoop(onSnoop);
#endif
    Bus.begin(JANDY_MY_ID, JANDY_SNIFF_ONLY, JANDY_PROMISCUOUS);

    xTaskCreatePinnedToCore(netTask, "net", 8192, NULL, 3, NULL, 0);
}

void loop() {
    // Heartbeat only. All real work happens in the two pinned tasks.
    static uint32_t last = 0;
    if (millis() - last > 15000) {
        last = millis();
        Serial.printf("[stat] online=%d packets=%lu bad_cksum=%lu heap=%u\n",
                      Bus.online(),
                      (unsigned long)Bus.packetsSeen(),
                      (unsigned long)Bus.badChecksums(),
                      (unsigned)ESP.getFreeHeap());
    }
    vTaskDelay(pdMS_TO_TICKS(500));
}
