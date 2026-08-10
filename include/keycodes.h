#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// All Button keypad keycodes
// ---------------------------------------------------------------------------
// !! READ THIS BEFORE TRUSTING THE TABLE BELOW !!
//
// These values vary by panel family (RS-4 / RS-6 / RS-8 / combo / dual) and I
// could not verify them against a panel. Treat them as a starting guess.
// There are two reliable ways to get the real numbers for YOUR panel:
//
//  1. Copy them out of AqualinkD's source/aq_serial.h, matching your model.
//     That file is the canonical table and is maintained against real
//     hardware.
//
//  2. Use the learn mode built into this firmware. RS-485 is a shared bus, so
//     we can see the real wall keypad's replies even though they are
//     addressed to the panel. Set JANDY_PROMISCUOUS=1, press a button on the
//     physical keypad, and watch the serial log for:
//
//         LEARN: keypad 0x08 pressed key 0x__
//
//     Press each button in turn and fill in the table. This takes about two
//     minutes and gives you values that are correct by construction.
//
// Getting these wrong is not dangerous -- a bad code is either ignored or
// toggles the wrong circuit -- but it will waste an afternoon if you assume
// the table is right.

#define KEY_NONE        0x00

#define KEY_FILTER_PUMP 0x02
#define KEY_SPA         0x01
#define KEY_AUX1        0x05
#define KEY_AUX2        0x06
#define KEY_AUX3        0x07
#define KEY_AUX4        0x08
#define KEY_AUX5        0x09
#define KEY_AUX6        0x0A
#define KEY_AUX7        0x0B
#define KEY_POOL_HEAT   0x0C
#define KEY_SPA_HEAT    0x0D
#define KEY_SOLAR_HEAT  0x0E

// Navigation keys, used for menu walking (temp setpoints, schedules).
#define KEY_MENU        0x10
#define KEY_CANCEL      0x11
#define KEY_LEFT        0x12
#define KEY_RIGHT       0x13
#define KEY_ENTER       0x14

// Index -> keycode, in the same order as the LED bitmap so that
// button N in the API maps to led[N] and BUTTON_KEYS[N].
static const uint8_t BUTTON_KEYS[] = {
    KEY_FILTER_PUMP,
    KEY_SPA,
    KEY_AUX1,
    KEY_AUX2,
    KEY_AUX3,
    KEY_AUX4,
    KEY_AUX5,
    KEY_AUX6,
    KEY_AUX7,
    KEY_POOL_HEAT,
    KEY_SPA_HEAT,
    KEY_SOLAR_HEAT,
};

static const char *BUTTON_NAMES[] = {
    "filter_pump",
    "spa",
    "aux1",
    "aux2",
    "aux3",
    "aux4",
    "aux5",
    "aux6",
    "aux7",
    "pool_heat",
    "spa_heat",
    "solar_heat",
};

static const size_t BUTTON_TABLE_LEN = sizeof(BUTTON_KEYS) / sizeof(BUTTON_KEYS[0]);
