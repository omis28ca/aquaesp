#pragma once
#include "jandy_protocol.h"
#include <stddef.h>

// ---------------------------------------------------------------------------
// Pure framing logic. No Arduino, no FreeRTOS, no globals.
//
// This is deliberately separated from jandy_serial.cpp so it builds and runs
// under `pio test -e native` on a laptop. Every protocol bug worth catching --
// checksums, DLE unstuffing, truncated frames, resync after noise -- is
// catchable here without a pool, a panel, or an ESP32.
//
// If you change framing behaviour, add a test in test/test_codec first.
// ---------------------------------------------------------------------------

class JandyDecoder {
public:
    struct Stats {
        uint32_t frames = 0;        // well-formed frames delivered
        uint32_t badChecksum = 0;
        uint32_t overflow = 0;      // frame longer than JANDY_MAX_DATA
        uint32_t resync = 0;        // truncated frame abandoned mid-stream
    };

    void reset();

    // Feed one received byte. Returns true exactly when `out` has been filled
    // with a complete, checksum-valid packet.
    bool feed(uint8_t b, JandyPacket &out);

    const Stats &stats() const { return _stats; }

private:
    enum RxState : uint8_t { S_IDLE, S_GOT_DLE, S_DATA, S_DATA_DLE };

    void push(uint8_t b);
    bool finish(JandyPacket &out);

    RxState _state = S_IDLE;
    uint8_t _raw[JANDY_MAX_DATA + 8] = {};  // DLE STX dest cmd data... cksum
    uint8_t _rawLen = 0;
    bool    _overflow = false;
    Stats   _stats;
};

// Build the fixed 11-byte keypad ACK:
//   NUL DLE STX 00 01 <ackType> <key> <cksum> DLE ETX NUL
// Returns bytes written, or 0 if cap is too small.
size_t jandy_build_ack(uint8_t *buf, size_t cap, uint8_t ackType, uint8_t key);

// Build an arbitrary frame with DLE stuffing applied to the payload and to the
// checksum byte. Used by the tests and by tools/fake_panel.py's C twin; you do
// not need this to act as a keypad, since ACK payloads can never contain 0x10.
// Returns bytes written, or 0 if cap is too small.
size_t jandy_encode(uint8_t *buf, size_t cap, uint8_t dest, uint8_t cmd,
                    const uint8_t *data, size_t len);
