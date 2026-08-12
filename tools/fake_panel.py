#!/usr/bin/env python3
"""
fake_panel.py -- pretend to be a Jandy Aqualink RS control panel.

Lets you develop and test the ESP32 firmware with no pool, no panel, and no
risk of putting a half-working device on a live bus.

Wiring: a USB-RS485 adapter from your laptop to the same A/B pair the ESP32's
transceiver is on. That's it -- two devices, one bus, you are the master.

    pip install pyserial
    ./fake_panel.py /dev/ttyUSB0 --keypad 0x0A

What it does:
  * polls the keypad ID with PROBE, STATUS and MSG frames the way a real panel
    does, roughly once every 200 ms
  * validates every ACK it gets back (framing, checksum, destination)
  * measures ACK turnaround and warns past 20 ms, errors past 60 ms
  * decodes keypresses the firmware sends and toggles simulated circuits,
    so LED state in the STATUS frames actually responds to your REST/MQTT calls

That last point is the useful one: it closes the loop. POST /api/button/2/on,
and if your keycode table and LED decode agree, this script will show aux1
turning on and the firmware will see it come back.
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial not installed:  pip install pyserial")

NUL, DLE, STX, ETX = 0x00, 0x10, 0x02, 0x03

CMD_PROBE, CMD_ACK, CMD_STATUS, CMD_MSG, CMD_MSG_LONG = 0x00, 0x01, 0x02, 0x03, 0x04

# Keep this in sync with include/keycodes.h. When you learn the real codes from
# your wall keypad, update both.
KEYMAP = {
    0x02: "filter_pump",
    0x01: "spa",
    0x05: "aux1",
    0x06: "aux2",
    0x07: "aux3",
    0x08: "aux4",
    0x09: "aux5",
    0x0A: "aux6",
    0x0B: "aux7",
    0x0C: "pool_heat",
    0x0D: "spa_heat",
    0x0E: "solar_heat",
}
BUTTON_ORDER = [
    "filter_pump", "spa", "aux1", "aux2", "aux3", "aux4",
    "aux5", "aux6", "aux7", "pool_heat", "spa_heat", "solar_heat",
]

# AqualinkD's RS-8 Combo button-to-LED mapping. LED indexes are one-based.
BUTTON_LED_INDEX = [7, 6, 5, 4, 3, 9, 8, 12, 1, 15, 17, 19]


def checksum(body: bytes) -> int:
    """Sum from DLE through the last data byte, & 0xFF."""
    return sum(body) & 0xFF


def encode(dest: int, cmd: int, data: bytes = b"") -> bytes:
    plain = bytes([DLE, STX, dest, cmd]) + data
    cks = checksum(plain)
    out = bytearray([NUL, DLE, STX])
    for b in plain[2:]:
        out.append(b)
        if b == DLE:
            out.append(NUL)          # stuff literal 0x10
    out.append(cks)
    if cks == DLE:
        out.append(NUL)
    out += bytes([DLE, ETX, NUL])
    return bytes(out)


class Decoder:
    """Mirror of lib/jandy_codec's state machine, so both ends agree."""

    IDLE, GOT_DLE, DATA, DATA_DLE = range(4)

    def __init__(self):
        self.state = self.IDLE
        self.raw = bytearray()

    def feed(self, b):
        """Yield (dest, cmd, data) for each valid frame."""
        if self.state == self.IDLE:
            if b == DLE:
                self.state = self.GOT_DLE
        elif self.state == self.GOT_DLE:
            if b == STX:
                self.raw = bytearray([DLE, STX])
                self.state = self.DATA
            elif b != DLE:
                self.state = self.IDLE
        elif self.state == self.DATA:
            if b == DLE:
                self.state = self.DATA_DLE
            else:
                self.raw.append(b)
        elif self.state == self.DATA_DLE:
            if b == ETX:
                self.state = self.IDLE
                return self._finish()
            elif b == NUL:
                self.raw.append(DLE)
                self.state = self.DATA
            elif b == STX:
                self.raw = bytearray([DLE, STX])
                self.state = self.DATA
            else:
                self.raw += bytes([DLE, b])
                self.state = self.DATA
        return None

    def _finish(self):
        if len(self.raw) < 5:
            return None
        if self.raw[-1] != checksum(self.raw[:-1]):
            print("  ! bad checksum from device:", self.raw.hex(" "))
            return None
        return (self.raw[2], self.raw[3], bytes(self.raw[4:-1]))


class FakePanel:
    def __init__(self, port, keypad_id, baud=9600):
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.keypad = keypad_id
        self.dec = Decoder()
        self.circuits = {name: False for name in BUTTON_ORDER}
        self.pool_temp = 78
        self.air_temp = 84
        self.acks = 0
        self.misses = 0
        self.worst_ms = 0.0

    def status_payload(self) -> bytes:
        """Five-byte AqualinkD LED bitmap, two bits per LED."""
        data = bytearray(5)
        for name, led_index in zip(BUTTON_ORDER, BUTTON_LED_INDEX):
            if self.circuits[name]:
                index = led_index - 1
                data[index // 4] |= 0x01 << ((index % 4) * 2)
        return bytes(data)

    def poll(self, cmd, data=b""):
        """Send one frame to the keypad and wait for its ACK."""
        self.ser.reset_input_buffer()
        self.ser.write(encode(self.keypad, cmd, data))
        self.ser.flush()

        t0 = time.perf_counter()
        deadline = t0 + 0.060          # the real panel's patience
        while time.perf_counter() < deadline:
            chunk = self.ser.read(32)
            for b in chunk:
                frame = self.dec.feed(b)
                if frame is None:
                    continue
                dest, rcmd, payload = frame
                if rcmd != CMD_ACK:
                    continue
                ms = (time.perf_counter() - t0) * 1000
                self.acks += 1
                self.worst_ms = max(self.worst_ms, ms)
                if ms > 20:
                    print(f"  ! slow ACK: {ms:.1f} ms "
                          f"(panel gives up at 60 ms)")
                if len(payload) >= 2 and payload[1] != 0:
                    self.handle_key(payload[1])
                return ms
        self.misses += 1
        print("  ! no ACK -- is JANDY_SNIFF_ONLY still 1? is the ID right?")
        return None

    def handle_key(self, code):
        name = KEYMAP.get(code)
        if name is None:
            print(f"  ? unknown keycode 0x{code:02X} "
                  f"(add it to KEYMAP and include/keycodes.h)")
            return
        self.circuits[name] = not self.circuits[name]
        print(f"  > key 0x{code:02X} -> {name} is now "
              f"{'ON' if self.circuits[name] else 'off'}")

    def run(self):
        print(f"faking a panel, polling keypad 0x{self.keypad:02X}. ctrl-c to stop.\n")
        messages = [
            lambda: f"POOL TEMP {self.pool_temp}",
            lambda: f"AIR TEMP {self.air_temp}",
            lambda: "AQUALINK RS8 REV T",
            lambda: time.strftime("%a %I:%M%p").upper()[:16],
        ]
        i = 0
        try:
            while True:
                self.poll(CMD_PROBE)
                time.sleep(0.05)
                self.poll(CMD_STATUS, self.status_payload())
                time.sleep(0.05)
                self.poll(CMD_MSG, messages[i % len(messages)]().encode("ascii"))
                i += 1
                time.sleep(0.10)

                if i % 20 == 0:
                    on = [n for n, v in self.circuits.items() if v] or ["none"]
                    print(f"[{self.acks} acks, {self.misses} missed, "
                          f"worst {self.worst_ms:.1f} ms] on: {', '.join(on)}")
        except KeyboardInterrupt:
            print(f"\nstopped. {self.acks} acks, {self.misses} missed, "
                  f"worst turnaround {self.worst_ms:.1f} ms")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("port", help="serial device, e.g. /dev/ttyUSB0 or COM3")
    ap.add_argument("--keypad", default="0x0A",
                    help="keypad ID the firmware is impersonating (default 0x0A)")
    ap.add_argument("--baud", type=int, default=9600)
    args = ap.parse_args()
    FakePanel(args.port, int(args.keypad, 0), args.baud).run()


if __name__ == "__main__":
    main()
