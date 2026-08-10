#include "panel_state.h"
#include <string.h>
#include <stdlib.h>

PanelModel Panel;

const char *ledStateName(LedState s) {
    switch (s) {
    case LED_OFF:        return "off";
    case LED_ON:         return "on";
    case LED_FLASH:      return "flash";
    case LED_SLOW_FLASH: return "slow_flash";
    }
    return "unknown";
}

void PanelModel::lock() {
    if (!_mux) _mux = xSemaphoreCreateMutex();
    xSemaphoreTake(_mux, portMAX_DELAY);
}
void PanelModel::unlock() { xSemaphoreGive(_mux); }

PanelState PanelModel::snapshot() {
    lock();
    PanelState copy = _s;
    unlock();
    return copy;
}

// ---------------------------------------------------------------------------
void PanelModel::handlePacket(const JandyPacket &p) {
    switch (p.cmd) {

    case CMD_STATUS:
        applyStatus(p);
        break;

    case CMD_MSG: {
        char text[17] = {0};
        size_t n = p.len < 16 ? p.len : 16;
        memcpy(text, p.data, n);
        applyMessage(text);
        break;
    }

    case CMD_MSG_LONG: {
        // data[0] is a line number; the text follows.
        if (p.len < 2) break;
        char text[17] = {0};
        size_t n = p.len - 1;
        if (n > 16) n = 16;
        memcpy(text, &p.data[1], n);
        applyMessage(text);
        break;
    }

    case CMD_PROBE:
    case CMD_PROBE_ALT:
        lock();
        _s.online = true;
        unlock();
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// LED bitmap
// ---------------------------------------------------------------------------
// Two bits per LED, four LEDs per byte, low bits first. This layout is what
// AqualinkD uses and matches the LED_* enum above.
//
// VERIFY THIS ON YOUR PANEL. Turn on exactly one circuit at the wall keypad,
// watch the /api/raw log for the CMD_STATUS payload, and confirm that the bit
// pair that changed lines up with the button index you expect. Panels with
// expansion power centers shift the higher indices.
void PanelModel::applyStatus(const JandyPacket &p) {
    lock();
    bool changed = false;
    for (int i = 0; i < PANEL_BUTTON_COUNT; i++) {
        int byteIdx = i / 4;
        if (byteIdx >= p.len) break;
        int shift = (i % 4) * 2;
        LedState v = (LedState)((p.data[byteIdx] >> shift) & 0x03);
        if (_s.led[i] != v) { _s.led[i] = v; changed = true; }
    }
    _s.lastUpdateMs = millis();
    unlock();
    if (changed) _revision++;
}

// ---------------------------------------------------------------------------
// Display text
// ---------------------------------------------------------------------------
// The panel cycles a handful of informational lines through the 16-char
// display. We scrape the ones that carry data we cannot get from the LEDs:
// temperatures, setpoints, and service mode.
//
// Exact wording varies by panel revision. Add cases as you see them in the
// log rather than guessing -- an unmatched line is harmless.

static bool startsWith(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

// Pull the first run of digits out of a line, optionally negative.
static int firstInt(const char *s, int fallback) {
    const char *p = s;
    while (*p && !isdigit((unsigned char)*p) &&
           !(*p == '-' && isdigit((unsigned char)p[1]))) p++;
    if (!*p) return fallback;
    return atoi(p);
}

void PanelModel::applyMessage(const char *text) {
    // Trim trailing spaces so comparisons and JSON stay clean.
    char t[17];
    strncpy(t, text, 16);
    t[16] = 0;
    for (int i = 15; i >= 0 && (t[i] == ' ' || t[i] == 0); i--) t[i] = 0;

    lock();
    bool changed = strcmp(_s.message, t) != 0;
    strncpy(_s.message, t, sizeof(_s.message) - 1);

    if (startsWith(t, "AIR TEMP")) {
        _s.airTempF = firstInt(t, _s.airTempF);
    } else if (startsWith(t, "POOL TEMP")) {
        _s.poolTempF = firstInt(t, _s.poolTempF);
    } else if (startsWith(t, "SPA TEMP")) {
        _s.spaTempF = firstInt(t, _s.spaTempF);
    } else if (startsWith(t, "AIR") && strstr(t, "POOL")) {
        // Combined "AIR nn POOL nn" line on some revisions.
        const char *pool = strstr(t, "POOL");
        _s.airTempF  = firstInt(t, _s.airTempF);
        if (pool) _s.poolTempF = firstInt(pool, _s.poolTempF);
    } else if (startsWith(t, "POOL HEAT")) {
        _s.poolSetpointF = firstInt(t, _s.poolSetpointF);
    } else if (startsWith(t, "SPA HEAT")) {
        _s.spaSetpointF = firstInt(t, _s.spaSetpointF);
    } else if (strstr(t, "SERVICE")) {
        _s.serviceMode = true;
    } else if (strstr(t, "AQUALINK") || strstr(t, "REV")) {
        strncpy(_s.panelModel, t, sizeof(_s.panelModel) - 1);
    }

    _s.lastUpdateMs = millis();
    unlock();

    if (changed) _revision++;
}
