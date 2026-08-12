#include <Arduino.h>
#include <AqualinkD.h>
#include "base_api.h"
#include "config.h"
#include "panel_state.h"

using aqualinkd::Bus;

namespace {

constexpr UBaseType_t PACKET_QUEUE_LENGTH = 24;

QueueHandle_t packetQueue = nullptr;

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

// Runs on the high-priority RS-485 task after the ACK has been queued for
// transmission. Copy only; formatting and Serial I/O belong on core 0.
void enqueuePacket(const aqualinkd::Packet& packet, void*) {
    if (packetQueue == nullptr || xQueueSend(packetQueue, &packet, 0) != pdTRUE) {
        BaseApi::noteDroppedPacket();
    }
}

void logPacket(const aqualinkd::Packet& packet) {
    Serial.printf("%8lu  %02X %-6s ", static_cast<unsigned long>(millis()),
                  packet.destination, commandName(packet.command));
    for (size_t i = 0; i < packet.dataLength; ++i) {
        Serial.printf("%02X ", packet.data[i]);
    }

    if (packet.command == aqualinkd::CMD_MSG ||
        packet.command == aqualinkd::CMD_MSG_LONG) {
        Serial.print(" |");
        const size_t textOffset = packet.command == aqualinkd::CMD_MSG_LONG &&
                                  packet.dataLength > 0 ? 1 : 0;
        for (size_t i = textOffset; i < packet.dataLength; ++i) {
            const char value = static_cast<char>(packet.data[i]);
            Serial.print(value >= 32 && value < 127 ? value : '.');
        }
        Serial.print('|');
    }
    Serial.println();

    if (packet.destination == aqualinkd::DEV_MASTER &&
        packet.command == aqualinkd::CMD_ACK && packet.dataLength >= 2 &&
        packet.data[1] != 0) {
        Serial.printf("LEARN: a keypad on this bus sent key 0x%02X\n",
                      packet.data[1]);
    }
}

void packetTask(void*) {
    aqualinkd::Packet packet;
    for (;;) {
        if (xQueueReceive(packetQueue, &packet, portMAX_DELAY) == pdTRUE) {
            logPacket(packet);
            Panel.handlePacket(packet);
            BaseApi::recordPacket(packet);
        }
    }
}

bool beginPacketProcessing() {
    packetQueue = xQueueCreate(PACKET_QUEUE_LENGTH, sizeof(aqualinkd::Packet));
    if (packetQueue == nullptr) {
        return false;
    }

    TaskHandle_t task = nullptr;
    if (xTaskCreatePinnedToCore(packetTask, "jandy-packets", 4096, nullptr, 2,
                                &task, 0) != pdPASS) {
        vQueueDelete(packetQueue);
        packetQueue = nullptr;
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Core assignment
// ---------------------------------------------------------------------------
//   core 1 : RS-485 task (created inside JandyBus::begin), high priority.
//            Blocks on uart_read_bytes, wakes on a frame, ACKs, parses.
//   core 0 : this netTask, plus the WiFi driver and AsyncTCP.
//
// Arduino's loopTask also lives on core 1 but at priority 1, so it can never
// hold off the RS-485 task. We leave it idle.




void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.printf("Jandy Aqualink RS bridge -- impersonating keypad 0x%02X%s\n",
                  JANDY_MY_ID, JANDY_SNIFF_ONLY ? " (SNIFF ONLY, not transmitting)" : "");

    const aqualinkd::BusConfig busConfig = {
        .uartNumber = JANDY_UART_NUM,
        .rxPin = JANDY_PIN_RX,
        .txPin = JANDY_PIN_TX,
        .dePin = JANDY_PIN_DE,
        .baud = JANDY_BAUD,
        .deviceId = JANDY_MY_ID,
        .sniffOnly = JANDY_SNIFF_ONLY != 0,
        .promiscuous = JANDY_PROMISCUOUS != 0,
        .echoWindowUs = JANDY_ECHO_WINDOW_US,
    };
    if (!beginPacketProcessing()) {
        Serial.println("FATAL: unable to initialize packet processing");
        return;
    }
    if (!BaseApi::start()) {
        Serial.println("ERROR: unable to initialize the HTTP API");
    }
    if (!Bus.begin(busConfig, enqueuePacket)) {
        Serial.println("FATAL: unable to initialize the Jandy RS-485 bus");
    }
}

void loop() {
    // Heartbeat only. All real work happens in the two pinned tasks.
    static uint32_t last = 0;
    if (millis() - last > 15000) {
        last = millis();
        const aqualinkd::BusStats stats = Bus.stats();
        Serial.printf("[stat] packets=%lu acks=%lu bad_cksum=%lu overflow=%lu "
                      "dropped=%lu ack_latency_us=%lu heap=%u\n",
                      static_cast<unsigned long>(stats.packetsReceived),
                      static_cast<unsigned long>(stats.acknowledgementsSent),
                      static_cast<unsigned long>(stats.checksumErrors),
                      static_cast<unsigned long>(stats.framesOverflowed),
                      static_cast<unsigned long>(BaseApi::droppedPackets()),
                      static_cast<unsigned long>(stats.acknowledgementLatencyUs),
                      ESP.getFreeHeap());
    }
    vTaskDelay(pdMS_TO_TICKS(500));
}
