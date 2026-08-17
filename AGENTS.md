# AGENTS.md

Instructions for a coding agent working on this repo. Read this before
changing anything.

## What this is

Firmware for an ESP32 that impersonates an **Jandy Aqualink RS8 Pool/Spa Combo Control System - 6687RLY keypad** on a Jandy
Aqualink RS-485 bus and re-exposes the pool panel as REST + MQTT.
PlatformIO, Arduino framework.

Protocol work is ported from [AqualinkD](https://github.com/aqualinkd/AqualinkD)


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


## Safety note

This talks to equipment that runs pumps and gas heaters. Two devices sharing
one bus ID makes the bus misbehave in confusing ways, and a wrong LED decode
makes on/off commands silently invert. Prefer sniff mode, prefer the fake
panel, and do not add features that write to the bus faster than the panel
polls.
