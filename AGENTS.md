# AGENTS.md

Instructions for a coding agent working on this repo. Read this before
changing anything.

## What this is

Firmware for an ESP32 that impersonates an **All Button RS8 keypad** on a Jandy
Aqualink RS-485 bus and re-exposes the pool panel as REST + WebSocket + MQTT.
PlatformIO, Arduino framework.

Protocol work is ported from [AqualinkD](https://github.com/aqualinkd/AqualinkD)
(`source/aq_serial.c`, `source/aq_panel.c`) and
[aquaweb](https://github.com/earlephilhower/aquaweb) (`protocol.md`). When a
protocol question comes up, check those before inventing an answer.

## The single most important constraint

A Jandy frame carries a **destination ID only** — there is no source field.
The panel therefore cannot detect a *late* reply, only a missing one. A slow
ACK is worse than no ACK: the panel has already moved on and your reply lands
against the wrong poll.

Budget is ~60 ms; target is under 20 ms. This is why:

- the RS-485 task is pinned to **core 1** at `configMAX_PRIORITIES - 4`
- WiFi, MQTT, HTTP and AsyncTCP are confined to **core 0**
  (`CONFIG_ASYNC_TCP_RUNNING_CORE=0` in `platformio.ini`)
- `JandyBus::dispatch()` transmits the ACK **before** calling the packet handler
- DE is driven by the UART peripheral in `UART_MODE_RS485_HALF_DUPLEX`, not by
  software GPIO toggling

**Do not** move network work onto core 1, add blocking calls to the packet
handler, lower the RS-485 task priority, or replace hardware DE with
`digitalWrite`. If you need to do work in response to a packet, queue it.

Two transceiver paths are supported. With a DE pin (`JANDY_PIN_DE >= 0`) the
UART drives direction in hardware and turnaround is deterministic. With an
auto-direction module (`JANDY_PIN_DE == -1`) the board flips direction itself,
the UART stays in plain mode, and echo suppression is active because some of
those boards do not mute their receiver during transmit. Keep both paths
working; do not delete the `#if` in `begin()`.

`Bus.ackLatencyUs()` is exposed in `/api/state` — if it climbs above ~20000,
something is stealing core 1. Treat that as a regression.

## Layout

```
lib/jandy_codec/     framing: checksum, DLE stuffing, RX state machine
                     NO Arduino, NO FreeRTOS. Builds natively. Unit tested.
src/jandy_serial.cpp UART + task + key queue. Hardware wrapper only.
src/panel_state.cpp  LED bitmap decode, display-text scraping.
src/net_api.cpp      REST, WebSocket, MQTT, HA discovery.
src/main.cpp         task wiring, snoop logging, learn mode.
include/config.h     pins, IDs, WiFi/MQTT — the only file a user should edit.
include/keycodes.h   keypress table. SEE WARNING BELOW.
test/test_codec/     Unity tests for the codec.
tools/fake_panel.py  bus master simulator for hardware-in-the-loop testing.
```

**Protocol logic belongs in `lib/jandy_codec`**, because that is the part that
can be tested without hardware. If you find yourself adding framing or parsing
to `jandy_serial.cpp`, move it down into the codec and write a test instead.

## Build and test

```bash
pio test -e native      # codec unit tests, no hardware needed. Run these.
pio run -e esp32dev     # compile firmware
pio run -e esp32dev -t upload
pio device monitor
```

`pio test -e native` must stay green. It is the only automated check in the
repo, so do not let it rot.

## What is verified and what is not

This matters more than usual here, because the author had no panel to test
against. Be honest in commit messages about which category you are in.

**Verified** (anchored to AqualinkD's source and to a unit test):

- Frame layout `[NUL] DLE STX <dest> <cmd> <data...> <cksum> DLE ETX [NUL]`
- Checksum = sum from DLE through last data byte, `& 0xFF`.
  `test_checksum_matches_known_ack` pins this to AqualinkD's hardcoded
  null-ACK (`10 02 00 01 00 00` → `0x13`). If that test ever fails, the
  framing understanding is wrong — do not "fix" it by changing the expected
  value.
- ACK is addressed to `DEV_MASTER` (0x00), not to our own ID
- Keypad IDs are 0x08–0x0B; 0x08 is typically the physical wall keypad
- 9600 8N1

**NOT verified — needs real hardware to confirm:**

- **`include/keycodes.h` values are a guess.** They vary by panel family
  (RS-4 / RS-6 / RS-8 / combo / dual). Do not build features that assume they
  are right. The learn mode in `main.cpp` reads the real codes off the bus by
  watching the physical keypad's ACKs; that is the source of truth.
- **The LED bit layout in `applyStatus()`** — two bits per LED, four per byte,
  low pair first. Plausible, unconfirmed. Panels with expansion power centers
  probably shift the higher indices.
- **Display-text prefixes in `applyMessage()`** — exact wording varies by panel
  revision. Add cases from observed logs; do not guess at new ones.

When you confirm one of these against real hardware, move it to the verified
list and say so in the commit.

## Testing without a pool

1. `pio test -e native` covers all framing logic.
2. `tools/fake_panel.py` is a bus master simulator. Put a USB-RS485 adapter on
   the same A/B pair as the ESP32 and run it. It polls, validates ACKs,
   measures turnaround, and toggles simulated circuits in response to
   keypresses — so `POST /api/button/2/on` produces a visible effect and comes
   back through STATUS. Use it before ever touching a real panel.
3. `JANDY_SNIFF_ONLY 1` in `config.h` makes the firmware listen and never
   transmit. This is the safe mode for a first connection to real equipment.

## House rules

- Two-space... no: **4-space indent, 100-column soft limit**, matching existing
  files.
- Comments explain *why*, especially timing and protocol constraints. The
  existing comments carry real information — do not strip them as noise.
- No dynamic allocation on the RS-485 path.
- `PANEL_BUTTON_COUNT` and the tables in `keycodes.h` must stay the same length
  and the same order as the LED bitmap. `buttonCount()` in `net_api.cpp` clamps
  to the shorter of the two; keep that guard.
- Keep `include/config.h` the only file a user needs to edit for a normal
  install. Anything else is a smell.

## Known gaps, roughly in priority order

See `docs/TASKS.md`.

## Safety note

This talks to equipment that runs pumps and gas heaters. Two devices sharing
one bus ID makes the bus misbehave in confusing ways, and a wrong LED decode
makes on/off commands silently invert. Prefer sniff mode, prefer the fake
panel, and do not add features that write to the bus faster than the panel
polls.
