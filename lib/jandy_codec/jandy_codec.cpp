#include "jandy_codec.h"
#include <string.h>

// Full reset: framing state AND counters. Counters are part of the object's
// observable state, so leaving them behind here made stats leak across
// reset() boundaries -- surprising in tests and misleading in the field.
void JandyDecoder::reset() {
    _state = S_IDLE;
    _rawLen = 0;
    _overflow = false;
    _stats = Stats();
}

void JandyDecoder::push(uint8_t b) {
    if (_rawLen < sizeof(_raw)) _raw[_rawLen++] = b;
    else _overflow = true;
}

bool JandyDecoder::feed(uint8_t b, JandyPacket &out) {
    switch (_state) {

    case S_IDLE:
        if (b == JANDY_DLE) _state = S_GOT_DLE;
        break;

    case S_GOT_DLE:
        if (b == JANDY_STX) {
            _rawLen = 0;
            _overflow = false;
            push(JANDY_DLE);            // retained so the checksum covers it
            push(JANDY_STX);
            _state = S_DATA;
        } else if (b == JANDY_DLE) {
            // stay armed; back-to-back DLEs happen on a noisy bus
        } else {
            _state = S_IDLE;
        }
        break;

    case S_DATA:
        if (b == JANDY_DLE) _state = S_DATA_DLE;
        else push(b);
        break;

    case S_DATA_DLE:
        if (b == JANDY_ETX) {
            bool ok = finish(out);
            _state = S_IDLE;
            return ok;
        }
        if (b == JANDY_NUL) {           // stuffed literal 0x10
            push(JANDY_DLE);
            _state = S_DATA;
            break;
        }
        if (b == JANDY_STX) {           // previous frame was truncated
            _stats.resync++;
            _rawLen = 0;
            _overflow = false;
            push(JANDY_DLE);
            push(JANDY_STX);
            _state = S_DATA;
            break;
        }
        push(JANDY_DLE);
        push(b);
        _state = S_DATA;
        break;
    }
    return false;
}

bool JandyDecoder::finish(JandyPacket &out) {
    // _raw = DLE STX dest cmd [data...] cksum
    if (_overflow) { _stats.overflow++; return false; }
    if (_rawLen < 5) { _stats.resync++; return false; }

    uint8_t got  = _raw[_rawLen - 1];
    uint8_t want = jandy_checksum(_raw, _rawLen - 1);
    if (got != want) { _stats.badChecksum++; return false; }

    out.dest = _raw[2];
    out.cmd  = _raw[3];
    out.len  = (uint8_t)(_rawLen - 5);     // minus DLE STX dest cmd cksum
    if (out.len > JANDY_MAX_DATA) out.len = JANDY_MAX_DATA;
    memcpy(out.data, &_raw[4], out.len);
    out.micros_received = 0;               // filled in by the caller

    _stats.frames++;
    return true;
}

// ---------------------------------------------------------------------------
size_t jandy_build_ack(uint8_t *buf, size_t cap, uint8_t ackType, uint8_t key) {
    if (cap < JANDY_ACK_LEN) return 0;
    size_t n = 0;
    buf[n++] = JANDY_NUL;            // turnaround padding, not checksummed
    buf[n++] = JANDY_DLE;
    buf[n++] = JANDY_STX;
    buf[n++] = DEV_MASTER;           // ACKs are addressed to the panel
    buf[n++] = CMD_ACK;
    buf[n++] = ackType;
    buf[n++] = key;
    buf[n++] = jandy_checksum(&buf[1], 6);
    buf[n++] = JANDY_DLE;
    buf[n++] = JANDY_ETX;
    buf[n++] = JANDY_NUL;
    return n;
}

// ---------------------------------------------------------------------------
size_t jandy_encode(uint8_t *buf, size_t cap, uint8_t dest, uint8_t cmd,
                    const uint8_t *data, size_t len) {
    // Checksum is computed over the UNSTUFFED bytes, then the whole payload
    // (checksum included) is stuffed on the way out.
    uint8_t plain[JANDY_MAX_DATA + 4];
    if (len > JANDY_MAX_DATA) return 0;

    size_t p = 0;
    plain[p++] = JANDY_DLE;
    plain[p++] = JANDY_STX;
    plain[p++] = dest;
    plain[p++] = cmd;
    for (size_t i = 0; i < len; i++) plain[p++] = data[i];
    uint8_t cks = jandy_checksum(plain, p);

    size_t n = 0;
    if (cap < 1) return 0;
    buf[n++] = JANDY_NUL;
    // DLE STX are frame markers and are never stuffed.
    if (n + 2 > cap) return 0;
    buf[n++] = JANDY_DLE;
    buf[n++] = JANDY_STX;
    for (size_t i = 2; i < p; i++) {         // dest, cmd, data
        if (n + 2 > cap) return 0;
        buf[n++] = plain[i];
        if (plain[i] == JANDY_DLE) buf[n++] = JANDY_NUL;
    }
    if (n + 4 > cap) return 0;
    buf[n++] = cks;
    if (cks == JANDY_DLE) buf[n++] = JANDY_NUL;
    buf[n++] = JANDY_DLE;
    buf[n++] = JANDY_ETX;
    buf[n++] = JANDY_NUL;
    return n;
}
