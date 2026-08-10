#pragma once
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Jandy Aqualink RS wire format
// ---------------------------------------------------------------------------
// Derived from AqualinkD (aq_serial.c/.h) and earlephilhower/aquaweb
// (protocol.md). 9600 8N1, half duplex, single master (the panel).
//
//   [NUL] DLE STX <dest> <cmd> <data...> <cksum> DLE ETX [NUL]
//
//   cksum = sum of every byte from DLE through the last data byte, & 0xFF
//           (the leading NUL is line-turnaround padding and is NOT summed)
//   A literal 0x10 inside <data> is stuffed as 0x10 0x00.
//
// Sanity check against AqualinkD's hardcoded null-ACK:
//   NUL DLE STX 00 01 00 00 13 DLE ETX NUL
//   0x10 + 0x02 + 0x00 + 0x01 + 0x00 + 0x00 = 0x13  ok
//
// Only <dest> is on the wire -- there is no source field. The panel therefore
// cannot detect a late reply, it can only notice a missing one. Budget is
// roughly 60ms from end-of-frame to your ACK; aim for well under 20ms.

static const uint8_t JANDY_NUL = 0x00;
static const uint8_t JANDY_DLE = 0x10;
static const uint8_t JANDY_STX = 0x02;
static const uint8_t JANDY_ETX = 0x03;

// --- Device IDs ------------------------------------------------------------
// Confirmed against AqualinkD's serial_logger output.
#define DEV_MASTER          0x00   // the control panel; ACKs are addressed here
#define DEV_KEYPAD_0        0x08   // All Button RS keypads: 0x08 .. 0x0B
#define DEV_KEYPAD_3        0x0B
#define DEV_IAQUALINK_0     0x30   // iAqualink / Aqualink Touch: 0x30 .. 0x33
#define DEV_ONETOUCH_0      0x40   // OneTouch: 0x40 .. 0x43
#define DEV_RSSA_0          0x48   // RS Serial Adapter
#define DEV_SWG             0x50   // salt water generator / AquaPure
#define DEV_PDA_0           0x60   // PDA: 0x60 .. 0x63
#define DEV_EPUMP_0         0x78   // Jandy VSP ePump: 0x78 .. 0x7B

// --- Commands (panel -> keypad) --------------------------------------------
#define CMD_PROBE           0x00   // "are you there" -- answer or be dropped
#define CMD_ACK             0x01   // used by us, keypad -> panel
#define CMD_STATUS          0x02   // LED bitmap for every circuit
#define CMD_MSG             0x03   // 16-char display line
#define CMD_MSG_LONG        0x04   // [line][16 chars] (OneTouch/Touch style)
#define CMD_PROBE_ALT       0x05

// --- ACK payload byte 0 (keypad -> panel) ----------------------------------
#define ACK_NORMAL          0x00
#define ACK_SCREEN_BUSY     0x01   // also sent on return from a long message
#define ACK_PAUSE           0x03

#define JANDY_MAX_DATA      48
#define JANDY_ACK_LEN       11

struct JandyPacket {
    uint8_t dest;
    uint8_t cmd;
    uint8_t data[JANDY_MAX_DATA];
    uint8_t len;                    // number of valid bytes in data[]
    uint32_t micros_received;
};

// Sum from DLE through the last data byte.
inline uint8_t jandy_checksum(const uint8_t *from_dle, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += from_dle[i];
    return (uint8_t)(sum & 0xFF);
}
