#pragma once

#include <Arduino.h>
#include <AqualinkDCodec.h>

#include "config.h"

enum class LedState : uint8_t {
    Off,
    On,
    Flash,
    Enabled,
    Unknown,
};

struct PanelSnapshot {
    LedState buttons[PANEL_BUTTON_COUNT];
    uint32_t revision;
    char display[17];
    uint32_t displayRevision;
};

class PanelModel {
public:
    PanelModel();

    // Decodes AqualinkD's five-byte All Button LED bitmap. Returns true only
    // when at least one published button state changes.
    bool handlePacket(const aqualinkd::Packet& packet);
    PanelSnapshot snapshot() const;
    uint32_t revision() const;

    static const char* buttonName(size_t index);
    static uint8_t keyCode(size_t index);
    static const char* stateName(LedState state);
    static bool isActive(LedState state);

private:
    mutable portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;
    LedState buttons_[PANEL_BUTTON_COUNT];
    uint32_t revision_ = 0;
    char display_[17] = {};
    uint32_t displayRevision_ = 0;
};

extern PanelModel Panel;
