#pragma once

// ---------------------------------------------------------------------------
// Hardware -- Waveshare ESP32-S3-RS485-CAN
// ---------------------------------------------------------------------------
// This project targets the Waveshare ESP32-S3-RS485-CAN industrial control
// board. It has an ISOLATED RS485 transceiver built in, so no external module
// is needed -- the Jandy bus lands directly on the A+/B- screw terminals.
//
// Wiring:
//   A+  <-> Jandy bus A (red)
//   B-  <-> Jandy bus B (black)
//   GND <-> Jandy bus common      <- do not skip this
//
// The 120R termination jumper stays at NC. We are a drop in the middle of an
// existing bus; the panel and the far-end device are the two endpoints. Only
// move it to 120R if you are physically at the end of the run.
//
// Do NOT power the board from the screw terminal and USB-C at the same time.
// Waveshare says this risks damaging the module. During development use USB
// only; for the permanent install use 12-24V on the terminal only.
//
// GPIO assignments are fixed by the board:
//   RS485 TX = 17,  RS485 RX = 18,  RS485 EN (DE) = 21
//   CAN   TX = 15,  CAN   RX = 16   <- do not reuse these
//
// Note GPIO16/17 were the defaults for a plain ESP32 devkit. On this board 16
// belongs to the CAN transceiver, which is why RX moved to 18.

#define JANDY_UART_NUM   UART_NUM_1
#define JANDY_PIN_RX     18
#define JANDY_PIN_TX     17
#define JANDY_BAUD       9600

// Driver enable. GPIO21 on this board, driven by the UART in hardware -- the
// deterministic path, and the one this project was designed around. Set to -1
// only if you move to an auto-direction module with no DE pin (that path is
// still supported; see the notes at the bottom of this file).
#define JANDY_PIN_DE     21

// Echo suppression. Relevant only on auto-direction modules that leave their
// receiver enabled during transmit. Harmless on this board, where the DE line
// mutes the receiver properly, but left active as a cheap safety net.
//
// This window must stay comfortably shorter than the gap between the panel
// polling us and the panel polling the real wall keypad, or learn mode will
// start discarding the physical keypad's keypresses as echoes.
#define JANDY_ECHO_WINDOW_US  15000

// ---------------------------------------------------------------------------
// Bus identity
// ---------------------------------------------------------------------------
// Which keypad ID we impersonate. Valid All Button IDs are 0x08..0x0B.
// 0x08 is almost always the real wall keypad -- pick a free one.
// Run with JANDY_SNIFF_ONLY=1 first (below) and watch the log to see which
// IDs the panel is already probing.
#define JANDY_MY_ID      0x0A

// Set to 1 to listen only and never transmit. Do this on first power-up:
// you get a full bus dump with zero risk of colliding with a live device.
#define JANDY_SNIFF_ONLY 1

// Log every packet on the bus (not just ours) to serial + /api/raw.
// Useful during bring-up; costs nothing on the RS-485 task.
#define JANDY_PROMISCUOUS 1

// ---------------------------------------------------------------------------
// Panel layout
// ---------------------------------------------------------------------------
// Number of controllable circuits your panel exposes. RS-8 = 12.
// Extra entries are harmless but will report as always-off.
#define PANEL_BUTTON_COUNT 12

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------
#define WIFI_SSID        "thats_what_she_ssid"
#define WIFI_PASS        "1234567890"
#define DEVICE_HOSTNAME  "pool-bridge"

#define MQTT_ENABLED     1
#define MQTT_HOST        "10.0.0.250"
#define MQTT_PORT        1883
#define MQTT_USER        ""          // "" for anonymous
#define MQTT_PASS        ""
#define MQTT_BASE_TOPIC  "jandy"
#define MQTT_HA_DISCOVERY_PREFIX "homeassistant"

#define HTTP_PORT        80
