# Jandy Aqualink RS protocol notes

Consolidated so the agent does not have to re-derive this. Sources:
[AqualinkD](https://github.com/aqualinkd/AqualinkD) `source/aq_serial.c` and
`source/aq_panel.c`, and [aquaweb](https://github.com/earlephilhower/aquaweb)
`protocol.md`. Where they disagree with what you observe on your own bus, your
bus wins — record the observation in `docs/captures/`.

## Physical layer

9600 baud, 8N1, half duplex, two-wire RS-485. One master (the control panel);
every other device is a poll-response slave.

## Frame

```
[NUL] DLE STX <dest> <cmd> <data...> <cksum> DLE ETX [NUL]

DLE = 0x10   STX = 0x02   ETX = 0x03   NUL = 0x00
```

- `cksum` = sum of every byte from `DLE` through the last data byte, `& 0xFF`.
  The leading `NUL` is line-turnaround padding and is **not** summed.
- A literal `0x10` inside the payload is stuffed as `0x10 0x00`.
- Leading and trailing `NUL` give the transceivers time to settle. Send them.

Anchor case, from AqualinkD's hardcoded null-ACK:

```
NUL DLE STX 00 01 00 00 13 DLE ETX NUL
0x10 + 0x02 + 0x00 + 0x01 + 0x00 + 0x00 = 0x13   ✓
```

`test_checksum_matches_known_ack` pins this. It is the load-bearing assumption
of the whole project.

## The timing rule

**There is no source field.** A frame says who it is *for*, never who it is
*from*. The panel can detect a missing reply but not a late one — a late ACK
arrives after the panel has moved on and gets attributed to the wrong poll,
which is worse than silence.

Roughly 60 ms of patience. AqualinkD's author targets under 20 ms. This single
fact is why the project is on an MCU rather than a Pi, and it constrains the
whole architecture (see AGENTS.md).

## Device IDs

| ID | Device |
|---|---|
| `0x00` | Master / control panel — **ACKs are addressed here** |
| `0x08`–`0x0B` | All Button RS keypads (0x08 is usually the physical one) |
| `0x30`–`0x33` | iAqualink / Aqualink Touch |
| `0x40`–`0x43` | OneTouch |
| `0x48` | RS Serial Adapter |
| `0x50` | Salt water generator / AquaPure |
| `0x60`–`0x63` | PDA |
| `0x78`–`0x7B` | Jandy VSP ePump |

Confirmed against AqualinkD's `serial_logger` output. Two devices on one ID
makes the bus misbehave — always sniff before claiming one.

## Commands, panel → keypad

| Cmd | Meaning |
|---|---|
| `0x00` | PROBE — answer or be dropped |
| `0x01` | ACK (this is what *we* send, keypad → panel) |
| `0x02` | STATUS — LED bitmap for every circuit |
| `0x03` | MSG — 16-char display line, plain ASCII |
| `0x04` | MSG_LONG — `[line][16 chars]` |
| `0x05` | PROBE, alternate form |

## Our reply

```
NUL DLE STX 00 01 <ackType> <keycode> <cksum> DLE ETX NUL
```

- `ackType`: `0x00` normal, `0x01` screen busy, `0x03` pause
- `keycode`: `0x00` for "no button pressed", otherwise one keypress

Neither field can be `0x10`, so the ACK never needs stuffing.

## The conceptual model

All intelligence lives in the panel. Keypads are dumb terminals: they render
what they are told and report at most **one keypress per received frame**.

Two consequences that shape the whole codebase:

1. **There is no "set circuit on" command.** A keypress *toggles*. To make the
   REST and MQTT surfaces idempotent, `applyButtonCommand()` reads the current
   LED state and only presses when a change is actually needed. If the LED
   decode is wrong, on/off commands silently do the opposite of what was asked.

2. **Anything not on an LED requires menu walking** — setpoints, schedules,
   configuration. You press `MENU`, read the display line to find out where you
   ended up, and navigate from there. It is screen-scraping a 16-character
   terminal, with all the fragility that implies. Build it as a state machine
   with timeouts, never as a blind keypress sequence.

## What we do not do

Act as master. There is exactly one, it is the panel, and replacing it means
driving pumps and gas heaters with none of the panel's interlocks.

## Square remote / SpaLink (different, older, not implemented)

aquaweb documents the older square remote at IDs `0x40`–`0x43` and SpaLink at
`0x20`–`0x23`, with a different command set (`0x09` clear screen,
`0x04[line][ascii]` write line, `0x0f` scroll, `0x08`/`0x10` invert) and ACKs
prefixed `0x8b`. Recorded here only so nobody confuses those command bytes with
the All Button ones above — they are not interchangeable.
