# Jandy Aqualink RS → ESP32 bridge

An ESP32 that impersonates an **All Button RS keypad** on the Jandy RS-485 bus
and re-exposes the panel as REST + WebSocket + MQTT.

Protocol work is ported from [AqualinkD](https://github.com/aqualinkd/AqualinkD)
(`aq_serial.c`, `aq_panel.c`) and [aquaweb](https://github.com/earlephilhower/aquaweb)
(`protocol.md`). Both are worth having open while you bring this up.

## Why an MCU instead of a Pi

The Jandy bus is master-polled and a frame carries only a destination ID — no
source field. The panel can't detect a *late* reply, only a missing one, so a
slow ACK is worse than no ACK. You have roughly 60 ms; AqualinkD's author aims
for under 20 ms. On the ESP32 the DE line is driven by the UART peripheral in
`UART_MODE_RS485_HALF_DUPLEX`, the RS-485 task is pinned to core 1 at high
priority, and WiFi/MQTT/HTTP are confined to core 0. Turnaround is the 11 ms it
takes to clock out 11 bytes at 9600 baud, plus microseconds of compute.

## Wiring

```
ESP32            RS-485 transceiver        Jandy bus
GPIO17 (TX) ---> DI
GPIO16 (RX) <--- RO
GPIO4  (DE) ---> DE + /RE (tied)
3V3         ---> VCC                       A ---- red
GND         ---> GND                       B ---- black
                                           GND -- shield/common
```

### If your module has no DE pin

Boards with only VCC/GND/RXD/TXD flip direction automatically by sensing the
start bit. They work here — 9600 baud is the easy case for them. Set
`JANDY_PIN_DE` to `-1` and the firmware drops out of hardware RS-485 mode and
turns on echo suppression.

**Check the chip marking, not the silkscreen.** A **MAX3485** is a 3.3 V part
(3.0–3.6 V, datasheet says don't exceed 3.6 V). Power it from the ESP32's 3V3
pin and you need no level shifting at all — RO swings 0–3.3 V, which is exactly
what the ESP32 wants. A **MAX485** is the 5 V part and its RO output will exceed
the ESP32's ~3.6 V limit; that one needs 1 kΩ series + 2 kΩ to ground on the
RO → RX line, or a BSS138 shifter. The two chips look almost identical on a
board, and module silkscreens are unreliable.

Running a 3.3 V transceiver on a bus shared with 5 V nodes is fine: RS-485 needs
only 1.5 V differential out and receivers trigger at ±200 mV.

Two things to watch once running: `ack_latency_us` in `/api/state` is no longer
deterministic (it depends on the module's release timing rather than on the
UART), and `echoes_dropped` tells you whether your board mutes its receiver
during transmit.

### Transceiver choice

Use a 3.3 V part — MAX3485, SP3485, or an isolated **ADM2483**. Isolation is the
right call: this bus runs out to pumps and heaters, and a ground loop through
your ESP32 is an annoying way to find that out. Only fit the 120 Ω terminator if
you're physically at the end of the run.

## Bring-up, in order

**1. Sniff first.** Set `JANDY_SNIFF_ONLY 1` in `include/config.h` and flash.
You'll transmit nothing. The serial monitor shows every frame on the bus:

```
   14231  08 STATUS 04 00 00 00 00 00
   14295  08 MSG    50 4F 4F 4C 20 54 45 4D 50 20 37 38  |POOL TEMP 78|
   14350  00 ACK    00 00
```

Watch which IDs the panel probes. `0x08` is almost certainly your wall keypad.
Anything in `0x30`–`0x33` is iAqualink or a Touch panel. Pick a **free** ID from
`0x09`/`0x0A`/`0x0B` and put it in `JANDY_MY_ID`. Two devices on one ID will
make the bus misbehave in ways that are irritating to diagnose.

**2. Learn your keycodes.** Still in sniff mode, press each button on the wall
keypad and watch for:

```
LEARN: a keypad on this bus sent key 0x05
```

RS-485 is a shared medium, so you see the real keypad's replies even though
they're addressed to the panel. Fill in `include/keycodes.h` from what you
observe. **The table shipped in that file is an unverified guess** — codes vary
by panel family, and I had no hardware to check against. Cross-reference
AqualinkD's `source/aq_serial.h` for your model as well.

**3. Verify the LED bit layout.** Turn on exactly one circuit at the keypad and
watch the `STATUS` payload. The decoder in `panel_state.cpp` assumes two bits
per LED, four LEDs per byte, low pair first. Confirm the pair that changed lines
up with the button index you expect — panels with expansion power centers shift
the higher indices.

**4. Go live.** Set `JANDY_SNIFF_ONLY 0`. The panel will probe your ID, you'll
start ACKing, and `online` flips true.

## Repo layout

`lib/jandy_codec/` holds all framing logic and has **no Arduino or FreeRTOS
dependency**, so it builds and unit-tests on a laptop. `src/jandy_serial.cpp`
is only the UART and threading wrapper. Keep protocol logic on the codec side
of that line — it is the difference between testable and not.

```
lib/jandy_codec/     framing: checksum, DLE stuffing, RX state machine
src/jandy_serial.cpp UART + pinned task + key queue
src/panel_state.cpp  LED decode, display scraping
src/net_api.cpp      REST / WebSocket / MQTT
include/config.h     the only file you should need to edit
test/test_codec/     Unity tests (12, all passing)
tools/fake_panel.py  bus master simulator
```

## Testing without a pool

```bash
pio test -e native      # framing tests, no hardware at all
```

For end-to-end testing, `tools/fake_panel.py` pretends to be the control panel.
Put a USB-RS485 adapter on the same A/B pair as the ESP32:

```bash
pip install pyserial
./tools/fake_panel.py /dev/ttyUSB0 --keypad 0x0A
```

It polls, validates your ACKs, measures turnaround, warns past 20 ms, and
toggles simulated circuits in response to keypresses — so `POST /api/button/2/on`
produces a visible effect that comes back through STATUS. Do this before you go
anywhere near real equipment.

## API

| Method | Path | Effect |
|---|---|---|
| GET  | `/api/state` | Full state JSON |
| POST | `/api/button/{n}/{on\|off\|toggle}` | Circuit control |
| POST | `/api/key/{code}` | Raw keypress, e.g. `/api/key/0x10` |
| GET  | `/api/raw` | Last 60 bus frames |
| WS   | `/ws` | State JSON pushed on every change |

MQTT publishes retained state to `jandy/state` and `jandy/button/<name>/state`,
subscribes to `jandy/button/<name>/set`, and emits Home Assistant discovery
under `homeassistant/`.

## The one conceptual thing to internalise

There is no "set circuit 3 on" command. A keypress **toggles**. `applyButtonCommand()`
in `net_api.cpp` reads the current LED state and only queues a press when the
state actually needs to change — that's what makes the REST and MQTT interfaces
idempotent. If your LED decode is wrong, on/off commands will silently do the
opposite of what you asked, which is why step 3 above matters more than it looks.

## Known gaps

- **Setpoints and schedules** need menu walking (`KEY_MENU`, arrows, `KEY_ENTER`)
  while scraping the display line to know where you are. Not implemented. The
  All Button keypad also can't program VSP pumps at all — that needs OneTouch
  (`0x40`–`0x43`) emulation, which is a separate and larger protocol.
- **Pump RPM/watts** live on `0x78`–`0x7B` and are visible in promiscuous mode
  but aren't decoded here.
- **SWG/chlorinator** state is on `0x50`, likewise visible but not decoded.
