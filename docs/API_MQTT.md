# REST and MQTT integration reference

This document describes the interfaces implemented by `src/base_api.cpp`.
Defaults shown here may be changed at compile time in `include/config.h`. Query
`GET /api/config` to discover the non-secret configuration of a running device.

The examples use:

- Device URL: `http://10.0.0.92`
- MQTT base topic: `jandy`

Replace both values with the values returned by your device.

## Safety and command model

An RS-8 circuit command is a physical-style **toggle**, not an absolute
on/off command. HTTP and MQTT only put a keypress into a 16-entry queue. The
RS-485 task attaches at most one queued key to the ACK for the next STATUS frame
addressed to this keypad.

A successful HTTP `202` or MQTT `queued` result means only that the key entered
the local queue. It does not prove that the control panel acted on it. Confirm
the resulting retained button state or a newer `button_revision`.

For automation:

1. Require `online: true` and `sniff_only: false`.
2. Read the current button state.
3. Send at most one toggle if the current state differs from the desired state.
4. Wait for a newer `button_revision` or changed button topic.
5. Do not retry blindly after a timeout.

This is especially important for pumps and heaters.

## REST API

The HTTP server has no authentication, authorization, TLS, or CORS
configuration. Use it only on a trusted local network.

### Endpoint summary

| Method | Path | Content type | Description |
|---|---|---|---|
| `GET` | `/` | `text/html` | Embedded RS-8 browser panel |
| `GET` | `/api/status` | `application/json` | Runtime, bus, Wi-Fi, and MQTT health |
| `GET` | `/api/state` | `application/json` | `/api/status` plus display and button state |
| `GET` | `/api/config` | `application/json` | Non-secret compile-time configuration |
| `GET` | `/api/raw` | `text/plain` | Oldest-to-newest ring of the last 60 decoded frames |
| `POST` | `/api/button?index=<0-11>` | `application/json` | Queue one mapped RS-8 circuit key |
| `POST` | `/api/key?code=<1-255>` | `application/json` | Queue one raw key code |

Numeric query parameters accept decimal or C-style hexadecimal such as `9` or
`0x09`. POST requests use query parameters and do not require a request body.

### `GET /api/status`

Example:

```json
{
  "online": true,
  "bus_running": true,
  "uptime_ms": 123456,
  "free_heap": 196000,
  "packets": 12345,
  "acks": 678,
  "bad_checksums": 0,
  "overflows": 0,
  "echoes_dropped": 0,
  "packets_dropped": 0,
  "ack_latency_us": 112,
  "keys_queued": 0,
  "sniff_only": false,
  "wifi_connected": true,
  "mqtt_connected": true,
  "ip": "10.0.0.92",
  "wifi_rssi": -58
}
```

| Field | Meaning |
|---|---|
| `online` | This keypad ID received a panel frame within the last 5 seconds |
| `bus_running` | The RS-485 task and UART are running |
| `uptime_ms` | Milliseconds since ESP32 boot |
| `free_heap` | Current free ESP32 heap in bytes |
| `packets` | Valid decoded Jandy frames since boot |
| `acks` | ACK frames submitted by this emulator since boot |
| `bad_checksums` | Frames rejected for checksum errors |
| `overflows` | Decoder frame-buffer overflows |
| `echoes_dropped` | Self-echo frames suppressed on auto-direction adapters |
| `packets_dropped` | Packets dropped at the core-1 to core-0 handoff |
| `ack_latency_us` | Processing latency from completed decode to ACK submission; keep below 20,000 us |
| `keys_queued` | Keypresses waiting for a STATUS reply, maximum 16 |
| `sniff_only` | `true` when all bus transmission and controls are disabled |
| `wifi_connected` | Wi-Fi station association state |
| `mqtt_connected` | MQTT is enabled and connected |
| `ip` | Present while Wi-Fi is connected |
| `wifi_rssi` | Wi-Fi RSSI in dBm; present while connected |

`online` is the value to use for command readiness. `bus_running` can remain
true when the panel is disconnected.

### `GET /api/state`

This contains every `/api/status` field plus:

```json
{
  "button_revision": 42,
  "display": " AIR TEMP 72 F  ",
  "display_revision": 17,
  "buttons": [
    {
      "index": 0,
      "name": "filter_pump",
      "state": "off",
      "on": false
    }
  ]
}
```

| Field | Meaning |
|---|---|
| `button_revision` | Increments when at least one decoded button state changes |
| `display` | Latest 16-character display message for this emulated keypad |
| `display_revision` | Increments when `display` changes |
| `buttons[].index` | Stable REST button index |
| `buttons[].name` | Stable MQTT and automation identifier |
| `buttons[].state` | `off`, `on`, `flash`, `enabled`, or `unknown` |
| `buttons[].on` | Convenience boolean; true for `on`, `flash`, and `enabled` |

Use the string state when heater-enabled and flashing distinctions matter. Use
the boolean only when every active state should be treated as on.

### Button map

| Index | Name | Physical label | Key code |
|---:|---|---|---:|
| 0 | `filter_pump` | Filter Pump | `0x02` |
| 1 | `spa` | Spa Mode | `0x01` |
| 2 | `aux1` | Aux 1 | `0x05` |
| 3 | `aux2` | Aux 2 | `0x0A` |
| 4 | `aux3` | Aux 3 | `0x0F` |
| 5 | `aux4` | Aux 4 | `0x06` |
| 6 | `aux5` | Aux 5 | `0x0B` |
| 7 | `aux6` | Aux 6 | `0x10` |
| 8 | `aux7` | Aux 7 | `0x15` |
| 9 | `pool_heat` | Pool Heat | `0x12` |
| 10 | `spa_heat` | Spa Heat | `0x17` |
| 11 | `solar_heat` | Solar/Extra Heat | `0x1C` |

Navigation key codes are Menu `0x09`, Cancel `0x0E`, Left `0x13`, Right
`0x18`, and Enter `0x1D`. Menu navigation is panel-dependent and should be
implemented as a display-driven state machine, never a blind timed sequence.
These mappings come from AqualinkD and should be verified against the physical
panel before unattended automation is enabled.

### `GET /api/config`

Returns the compiled hostname, keypad ID, bus mode, UART settings, panel size,
HTTP port, and MQTT host/port/base topic. Wi-Fi and MQTT passwords are
intentionally omitted. The MQTT host itself is not treated as a secret.

### `GET /api/raw`

Returns up to 60 decoded frames, one per line:

```text
   12345  0A STATUS 00 00 00 00 00
   12520  0A MSG    00 20 41 49 52 20 54 45 4D 50 20 37 32 DF 46 00 44  |. AIR TEMP 72.F.D|
```

The first value is ESP32 milliseconds since boot, followed by destination ID,
decoded command name, and payload bytes. Display frames also include a printable
ASCII rendering. This endpoint is diagnostic, not a stable machine-state API.

### Command responses

Successful HTTP queue response:

```http
HTTP/1.1 202 Accepted
Content-Type: application/json

{"result":"queued","key":2}
```

| Status | Meaning |
|---:|---|
| `202` | Key accepted into the local queue |
| `400` | Missing, malformed, or out-of-range `index`/`code` |
| `403` | Firmware is in sniff-only mode |
| `404` | Route does not exist |
| `409` | This emulated keypad ID has not received a panel frame in 5 seconds |
| `503` | The 16-entry key queue is full |

Examples:

```powershell
# Read aggregate state
Invoke-RestMethod http://10.0.0.92/api/state

# Queue Filter Pump index 0 (a toggle, not an absolute ON)
Invoke-RestMethod -Method Post 'http://10.0.0.92/api/button?index=0'

# Queue the Menu key in hexadecimal
Invoke-RestMethod -Method Post 'http://10.0.0.92/api/key?code=0x09'
```

## MQTT

MQTT is optional at compile time. The client ID is `DEVICE_HOSTNAME`. It uses
the configured username/password when a username is present, otherwise it
connects anonymously. The implementation uses PubSubClient defaults: QoS 0 for
publishes and the command subscription, with no TLS.

All topics are below `MQTT_BASE_TOPIC`, which defaults to `jandy`.

### Topic reference

| Topic | Direction | Retained | Payload and publication behavior |
|---|---|---:|---|
| `jandy/status` | Device to broker | yes | `online`; broker last will publishes `offline` |
| `jandy/state` | Device to broker | yes | Same JSON object as `/api/state`; published on connect and button revision changes |
| `jandy/button/<name>/state` | Device to broker | yes | `off`, `on`, `flash`, `enabled`, or `unknown`; published on connect and when that button changes |
| `jandy/raw` | Device to broker | no | Latest formatted decoded frame; high-volume diagnostic stream |
| `jandy/key/set` | Automation to device | no | One raw key code in decimal or `0x` hexadecimal |
| `jandy/key/result` | Device to broker | no | `queued`, `queue_full`, `invalid_key`, or `sniff_only` |

Substitute the configured base topic for `jandy`.

Important publication details:

- `state` and per-button topics are initialized after every MQTT reconnect.
- `state` is republished when the button revision changes, not periodically.
- Display-only changes do **not** trigger a new retained `state` publication.
  Poll `/api/state` or consume `raw` if live keypad display text is required.
- `raw` may publish every decoded bus frame. Do not use it as the primary
  automation state source.
- `key/result` is a shared, uncorrelated result topic. It has no request ID and
  confirms queue admission only.
- The MQTT command callback currently does not reject an offline panel. Check
  retained `state.online` before publishing; otherwise a key may remain queued
  until the keypad receives STATUS traffic.

### MQTT command examples

```text
Topic:   jandy/key/set
Payload: 2
```

queues the Filter Pump key code. Hexadecimal is also accepted:

```text
Topic:   jandy/key/set
Payload: 0x02
```

Observe:

```text
jandy/key/result                 queued
jandy/button/filter_pump/state   on
```

The second message is the confirmation that matters. A `queued` result alone
must not be treated as proof that the equipment changed state.

Example command-line workflow:

```powershell
mosquitto_sub -h BROKER -t 'jandy/status' -t 'jandy/state' -t 'jandy/key/result' -v
mosquitto_pub -h BROKER -t 'jandy/key/set' -m '0x09'
```

### Recommended automation startup

1. Subscribe before publishing commands.
2. Wait for retained `jandy/status = online`.
3. Parse retained `jandy/state` and require `online = true`,
   `sniff_only = false`, and a current non-`unknown` button state.
4. Serialize key commands; do not publish concurrent retries.
5. After a circuit key, wait for its retained button topic or a newer
   `button_revision`.
6. Treat disconnects, `unknown`, and timeouts as indeterminate state, not off.

## Interface stability

Button indexes, button names, state strings, topic suffixes, and documented JSON
field names are intended as integration identifiers. Raw frame text and display
contents are diagnostic/panel-controlled data and should not be parsed as a
stable automation protocol.
