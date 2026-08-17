#include "panel_state.h"

namespace {

constexpr size_t STATUS_BYTE_COUNT = 5;
constexpr size_t LED_COUNT = STATUS_BYTE_COUNT * 4;
constexpr uint8_t KEYPAD_ID_MIN = 0x08;
constexpr uint8_t KEYPAD_ID_MAX = 0x0B;
static_assert(PANEL_BUTTON_COUNT == 12,
              "The current LED map is specifically for an RS-8 Combo panel");

// RS-8 Combo button order mapped to AqualinkD's one-based LED indexes.
constexpr uint8_t BUTTON_LED_INDEXES[PANEL_BUTTON_COUNT] = {
    7, 6, 5, 4, 3, 9, 8, 12, 1, 15, 17, 19,
};

constexpr const char* BUTTON_NAMES[PANEL_BUTTON_COUNT] = {
    "filter_pump", "spa", "aux1", "aux2", "aux3", "aux4",
    "aux5", "aux6", "aux7", "pool_heat", "spa_heat", "solar_heat",
};

// AqualinkD All Button key codes for an RS-8 Combo panel.
constexpr uint8_t BUTTON_KEY_CODES[PANEL_BUTTON_COUNT] = {
    0x02, 0x01, 0x05, 0x0A, 0x0F, 0x06,
    0x0B, 0x10, 0x15, 0x12, 0x17, 0x1C,
};

LedState decodeLed(const uint8_t* status, size_t oneBasedIndex) {
    if (oneBasedIndex == 0 || oneBasedIndex > LED_COUNT) {
        return LedState::Unknown;
    }
    const size_t index = oneBasedIndex - 1;
    const uint8_t pair = static_cast<uint8_t>(
        (status[index / 4] >> ((index % 4) * 2)) & 0x03);
    if ((pair & 0x02) != 0) {
        return LedState::Flash;
    }
    return (pair & 0x01) != 0 ? LedState::On : LedState::Off;
}

LedState decodeButton(const uint8_t* status, size_t buttonIndex) {
    const size_t ledIndex = BUTTON_LED_INDEXES[buttonIndex];
    LedState state = decodeLed(status, ledIndex);

    // All Button heaters use the following LED as an enabled/call indicator.
    if (buttonIndex >= 9 && state == LedState::Off &&
        decodeLed(status, ledIndex + 1) == LedState::On) {
        state = LedState::Enabled;
    }
    return state;
}

}  // namespace

PanelModel Panel;

PanelModel::PanelModel() {
    for (LedState& state : buttons_) {
        state = LedState::Unknown;
    }
}

bool PanelModel::handlePacket(const aqualinkd::Packet& packet) {
    if (packet.command == aqualinkd::CMD_MSG ||
        packet.command == aqualinkd::CMD_MSG_LONG) {
        if (packet.destination != JANDY_MY_ID) {
            return false;
        }

        const size_t offset = packet.command == aqualinkd::CMD_MSG_LONG ? 1 : 0;
        if (packet.dataLength <= offset) {
            return false;
        }

        char decoded[17] = {};
        const size_t length = min<size_t>(16, packet.dataLength - offset);
        for (size_t i = 0; i < length; ++i) {
            const char value = static_cast<char>(packet.data[offset + i]);
            decoded[i] = value >= 32 && value < 127 ? value : ' ';
        }

        bool changed = false;
        portENTER_CRITICAL(&mutex_);
        if (strncmp(display_, decoded, sizeof(display_)) != 0) {
            memcpy(display_, decoded, sizeof(display_));
            ++displayRevision_;
            changed = true;
        }
        portEXIT_CRITICAL(&mutex_);
        return changed;
    }

    if (packet.command != aqualinkd::CMD_STATUS ||
        packet.dataLength < STATUS_BYTE_COUNT ||
        packet.destination < KEYPAD_ID_MIN ||
        packet.destination > KEYPAD_ID_MAX) {
        return false;
    }

    LedState decoded[PANEL_BUTTON_COUNT];
    for (size_t i = 0; i < PANEL_BUTTON_COUNT; ++i) {
        decoded[i] = decodeButton(packet.data, i);
    }

    bool changed = false;
    portENTER_CRITICAL(&mutex_);
    for (size_t i = 0; i < PANEL_BUTTON_COUNT; ++i) {
        if (buttons_[i] != decoded[i]) {
            buttons_[i] = decoded[i];
            changed = true;
        }
    }
    if (changed) {
        ++revision_;
    }
    portEXIT_CRITICAL(&mutex_);
    return changed;
}

PanelSnapshot PanelModel::snapshot() const {
    PanelSnapshot result = {};
    portENTER_CRITICAL(&mutex_);
    for (size_t i = 0; i < PANEL_BUTTON_COUNT; ++i) {
        result.buttons[i] = buttons_[i];
    }
    result.revision = revision_;
    memcpy(result.display, display_, sizeof(result.display));
    result.displayRevision = displayRevision_;
    portEXIT_CRITICAL(&mutex_);
    return result;
}

uint32_t PanelModel::revision() const {
    portENTER_CRITICAL(&mutex_);
    const uint32_t value = revision_;
    portEXIT_CRITICAL(&mutex_);
    return value;
}

const char* PanelModel::buttonName(size_t index) {
    return index < PANEL_BUTTON_COUNT ? BUTTON_NAMES[index] : "unknown";
}

uint8_t PanelModel::keyCode(size_t index) {
    return index < PANEL_BUTTON_COUNT ? BUTTON_KEY_CODES[index] : 0;
}

const char* PanelModel::stateName(LedState state) {
    switch (state) {
        case LedState::Off:     return "off";
        case LedState::On:      return "on";
        case LedState::Flash:   return "flash";
        case LedState::Enabled: return "enabled";
        case LedState::Unknown: return "unknown";
    }
    return "unknown";
}

bool PanelModel::isActive(LedState state) {
    return state == LedState::On || state == LedState::Flash ||
           state == LedState::Enabled;
}
