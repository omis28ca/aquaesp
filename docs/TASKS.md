# TASKS

Ordered by what unblocks the most. Anything marked **needs hardware** cannot be
finished from the IDE alone — the agent can write the code and the tests, but a
human with a panel has to confirm it.

## 1. Confirm the unverified assumptions — needs hardware

Nothing below this line is trustworthy until these are settled. See AGENTS.md
for what is verified and what is not.

- [ ] Run with `JANDY_SNIFF_ONLY 1`, capture 5 minutes of bus traffic, save it
      to `docs/captures/` as raw hex lines.
- [ ] Learn the real keycodes from the physical keypad (learn mode prints
      `LEARN: a keypad on this bus sent key 0x__`). Update `include/keycodes.h`
      and the `KEYMAP` in `tools/fake_panel.py` together.
- [ ] Confirm the LED bit layout: turn on one circuit at a time, diff the
      STATUS payloads, verify the bit pair matches the button index.
- [ ] Record the actual display-line wording this panel emits and fix the
      prefixes in `applyMessage()`.

## 2. Replay harness — no hardware needed

- [ ] Add `test/test_panel/` covering `PanelModel` directly: feed synthetic
      STATUS and MSG packets, assert LED states and parsed temperatures.
      Requires splitting `PanelModel` off Arduino (`millis()` is the only
      dependency — inject a clock).
- [ ] Add a `tools/replay.py` that pipes a captured hex log into the native
      test binary, so real captures become regression fixtures.

## 3. Robustness

- [ ] Detect and report ID collision: if we see a CMD_ACK addressed to master
      that we did not send while our ID is being polled, another device is
      squatting. Log loudly and consider dropping offline.
- [ ] Watchdog on the RS-485 task — if no packet for our ID arrives for N
      seconds, clear `online` and mark MQTT availability offline.
- [ ] Persist `config.h` values to NVS with a captive-portal or web-based setup
      page, so a rebuild is not needed to change WiFi or the keypad ID.
- [ ] OTA update.

## 4. Features

- [ ] **Setpoint control.** Needs menu walking: `KEY_MENU`, arrows, `KEY_ENTER`
      while scraping the display line to know where in the menu you are. This
      is a state machine with timeouts, not a sequence of blind keypresses —
      the panel's menu position is not observable except through the text.
- [ ] **Schedules.** Same mechanism, more screens.
- [ ] **Pump telemetry.** RPM/watts live on `0x78`–`0x7B` and are already
      visible in promiscuous mode. Decoding is additive and low risk — a good
      first task since it is read-only.
- [ ] **SWG / chlorinator.** ID `0x50`. Also read-only, also low risk.
- [ ] **OneTouch emulation** (`0x40`–`0x43`). Substantially larger protocol.
      Needed for VSP pump programming, which All Button genuinely cannot do.
      Do not start this until section 1 is complete.

## 5. Nice to have

- [ ] Small served web UI (currently API only).
- [ ] Prometheus `/metrics` endpoint.
- [ ] Home Assistant climate entity for pool/spa heat once setpoints work.

## Explicitly out of scope

- Acting as bus **master**. There is exactly one master and it is the panel.
  Replacing it means driving pumps and heaters directly, with no interlocks.
- Anything that writes to the bus faster than the panel polls.
