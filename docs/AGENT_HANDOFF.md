# Agent handoff: direct ESP32 and pool-equipment bring-up

Date: 2026-08-13

## Start here

This repository is firmware for a Waveshare ESP32-S3-RS485-CAN board that
impersonates a Jandy Aqualink RS-8 Combo All Button keypad. Read
[`AGENTS.md`](../AGENTS.md) before changing or uploading anything. Its ACK
timing and core-affinity rules are mandatory.

The source of truth is the latest `main` branch on
`https://github.com/omis28ca/aquaesp.git`.

Suggested first prompt on the new PC:

> Read AGENTS.md and docs/AGENT_HANDOFF.md. Verify the checkout and build the
> waveshare_s3_rs485 environment. The PC is connected directly to the ESP32 and
> pool equipment. Keep sniff-only mode enabled until the bus IDs, key codes,
> and LED mapping have been captured and checked. Do not upload live-control
> firmware without showing the checks first.

## Current implementation

- Portable AqualinkD-derived Jandy framing/codec library.
- ESP32 UART transport with hardware DE and a high-priority core-1 ACK task.
- Core-0 packet processing, Wi-Fi, HTTP, MQTT, and AsyncTCP.
- HTTP status/configuration/raw-frame APIs.
- MQTT availability, raw frames, queued keys, aggregate state, and retained
  per-button state updates.
- Thread-safe RS-8 Combo LED decoder using AqualinkD's five-byte layout.
- A self-contained RS-8 web panel served from `/` with:
  - 12 circuit buttons and live LED states.
  - 16-character keypad display.
  - Menu, Cancel, Left, Right, and Enter keys.
  - Bus, ACK-latency, packet, key-queue, and Wi-Fi indicators.
- `tools/fake_panel.py` for isolated end-to-end testing.

The web UI is embedded in `include/web_ui.h`; no filesystem image or internet
connection is required.

## Last validation on the old PC

- Environment: `waveshare_s3_rs485`.
- Firmware build succeeded.
- RAM: about 81,164 bytes (24.8%).
- Flash: about 1,274,512 bytes (38.1%).
- Firmware uploaded successfully over COM10.
- ESP32 MAC observed during upload: `a0:f2:62:e3:c5:30`.
- Last DHCP address on the old LAN: `10.0.0.92` (do not assume it remains the
  same on the new connection).
- Browser UI and `/api/state` were validated.
- In sniff-only mode, `POST /api/button?index=0` correctly returned HTTP 403.
- No live pool-equipment command was sent or validated during this work.

## Critical safety state

`JANDY_SNIFF_ONLY` is currently `1` in `include/config.h`. Leave it at `1` for
the first connection to the real pool bus. In this mode the firmware listens
but never ACKs or sends key presses, and the web UI disables all controls.

A Jandy frame has a destination but no source. A late ACK can collide with the
next poll and is worse than a missing ACK. Preserve all of these constraints:

- RS-485 task pinned to core 1 at `configMAX_PRIORITIES - 4`.
- Network and application work on core 0.
- ACK sent before the packet callback.
- Packet callback only performs a zero-timeout queue copy.
- Hardware UART DE when `JANDY_PIN_DE >= 0`; retain the auto-direction path.
- No blocking calls, logging, JSON, HTTP, or MQTT in the RS-485 callback.
- Treat ACK latency above 20,000 microseconds as a regression.

Two devices must never share a keypad ID. `0x08` is normally the physical wall
keypad. The emulator currently uses `JANDY_MY_ID 0x0A`, but this must be proven
free on the actual bus before changing out of sniff-only mode.

## First-session procedure on the new PC

1. Clone/pull `main`, open the repository in VS Code, and install PlatformIO.
2. Build without uploading:
   - `pio run -e waveshare_s3_rs485`
3. Connect USB and identify the ESP32 serial port.
4. Confirm `JANDY_SNIFF_ONLY` is still `1`.
5. Upload and monitor:
   - `pio run -e waveshare_s3_rs485 -t upload`
   - `pio device monitor --baud 115200`
6. Save at least five minutes of bus traffic under `docs/captures/`.
7. Identify which keypad IDs from `0x08` through `0x0B` are being polled and
   answered. Confirm `JANDY_MY_ID` is unused.
8. Press every physical RS-8 button once and record each `LEARN` key code.
9. Turn on one circuit at a time and verify the corresponding two-bit LED pair
   in each five-byte STATUS payload.
10. Compare the captures against the maps below. Fix and rebuild if anything
    differs.
11. Only after those checks, set `JANDY_SNIFF_ONLY` to `0`, upload, and watch
    ACK latency before using web or MQTT controls.

Do not connect a fake panel and the real control panel as competing bus masters.
Use `tools/fake_panel.py` only on an isolated test bus.

## RS-8 mappings currently implemented

Button order, AqualinkD key code, and one-based LED index:

| Index | Name | Key | LED |
|---:|---|---:|---:|
| 0 | Filter Pump | `0x02` | 7 |
| 1 | Spa Mode | `0x01` | 6 |
| 2 | Aux 1 | `0x05` | 5 |
| 3 | Aux 2 | `0x0A` | 4 |
| 4 | Aux 3 | `0x0F` | 3 |
| 5 | Aux 4 | `0x06` | 9 |
| 6 | Aux 5 | `0x0B` | 8 |
| 7 | Aux 6 | `0x10` | 12 |
| 8 | Aux 7 | `0x15` | 1 |
| 9 | Pool Heat | `0x12` | 15 |
| 10 | Spa Heat | `0x17` | 17 |
| 11 | Solar/Extra Heat | `0x1C` | 19 |

Navigation codes are Menu `0x09`, Cancel `0x0E`, Left `0x13`, Right `0x18`,
and Enter `0x1D`.

The key values match AqualinkD's `source/aq_serial.h`, and the LED indexes match
its RS-8 Combo panel map. They still need comparison with this physical panel.
Heater `enabled` state is represented by the LED immediately following the
primary heater LED.

## Web and API behavior

Once Wi-Fi connects, serial output prints the assigned IP and web UI URL.
Hostname resolution may not work on Windows; use the printed DHCP address.

| Method | Path | Result |
|---|---|---|
| GET | `/` | RS-8 web panel |
| GET | `/api/status` | Runtime and network status |
| GET | `/api/state` | Runtime status, display, and 12 buttons |
| GET | `/api/config` | Non-secret compiled configuration |
| GET | `/api/raw` | Last 60 decoded frames |
| POST | `/api/button?index=0` | Queue one circuit-button press |
| POST | `/api/key?code=9` | Queue one raw/menu key press |

Control responses:

- `202`: queued for the ACK to the next STATUS frame addressed to this keypad.
- `403`: sniff-only mode.
- `409`: this emulated keypad is not being polled.
- `503`: key queue full.

A circuit command is a toggle, not an absolute on/off operation. The UI sends
one physical-style press and waits for the next STATUS frame to update the LED.
Do not retry a click blindly when the bus is delayed.

## MQTT

Topics below the configured base topic (currently `jandy`) include:

- `jandy/status`
- `jandy/state`
- `jandy/button/<name>/state`
- `jandy/raw`
- `jandy/key/set`
- `jandy/key/result`

Button topics and aggregate state publish after actual button-state changes,
with one retained initialization after MQTT reconnect. MQTT and HTTP only queue
keys; the core-1 bus task transmits at most one key with the ACK to the next
STATUS frame addressed to this keypad.

## Important files

- `AGENTS.md`: mandatory architecture and safety constraints.
- `include/config.h`: pins, keypad ID, sniff mode, Wi-Fi, and MQTT settings.
- `src/main.cpp`: task setup and packet handoff.
- `src/base_api.cpp`: Wi-Fi, HTTP routes, MQTT, and control validation.
- `include/web_ui.h`: embedded RS-8 browser interface.
- `include/panel_state.h`, `src/panel_state.cpp`: LED, display, and key mapping.
- `lib/aqualinkd_codec/`: portable framing and decoder.
- `lib/aqualinkd_esp32/`: UART, ACK path, and key queue.
- `tools/fake_panel.py`: isolated fake control-panel test harness.
- `docs/PROTOCOL.md`: framing and protocol notes.
- `docs/TASKS.md`: remaining verification and feature work.

## Known limitations and next work

- Physical key codes and LED positions have not yet been validated against the
  user's actual RS-8 equipment.
- No captured real-bus fixture exists yet.
- No native unit tests cover `PanelModel` yet.
- Setpoints and schedules require safe menu-walking state machines.
- Pump and chlorinator telemetry are visible on the bus but not decoded.
- The HTTP UI has no authentication; keep it on a trusted local network.
- Wi-Fi and MQTT secrets are currently compile-time configuration. Do not paste
  them into chat or logs, and rotate them if the repository exposure changes.
