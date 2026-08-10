#pragma once
#include <Arduino.h>
#include "jandy_codec.h"
#include "config.h"

enum LedState : uint8_t {
    LED_OFF        = 0,
    LED_ON         = 1,
    LED_FLASH      = 2,   // "enabled but waiting" -- e.g. heater called, not firing
    LED_SLOW_FLASH = 3,   // delay / countdown
};

struct PanelState {
    LedState led[PANEL_BUTTON_COUNT] = {};

    char  message[17]   = {0};    // current 16-char display line
    char  panelModel[24] = {0};   // scraped from the boot banner if we see it

    int   airTempF      = -999;
    int   poolTempF     = -999;
    int   spaTempF      = -999;
    int   poolSetpointF = -999;
    int   spaSetpointF  = -999;

    bool  serviceMode   = false;
    bool  online        = false;

    uint32_t lastUpdateMs = 0;
};

class PanelModel {
public:
    void handlePacket(const JandyPacket &p);   // called from the RS-485 task

    // Snapshot under a mutex -- safe to call from the network core.
    PanelState snapshot();

    // Bumped on every change worth pushing to MQTT/WebSocket clients.
    uint32_t revision() const { return _revision; }

private:
    void applyStatus(const JandyPacket &p);
    void applyMessage(const char *text);

    PanelState _s;
    SemaphoreHandle_t _mux = nullptr;
    volatile uint32_t _revision = 0;
    void lock();
    void unlock();
};

extern PanelModel Panel;

const char *ledStateName(LedState s);
