#pragma once
#include <Arduino.h>
#include <functional>
#include "jandy_codec.h"

// Half-duplex RS-485 client for the Jandy bus.
//
// Owns a FreeRTOS task pinned to core 1 at high priority so that WiFi, MQTT
// and HTTP work on core 0 can never delay an ACK. The task spends virtually
// all of its time blocked in uart_read_bytes().
//
// All framing lives in lib/jandy_codec (which builds natively and is unit
// tested). This class is only the hardware and threading wrapper -- keep
// protocol logic out of it so it stays testable.

class JandyBus {
public:
    // Called for every well-formed packet addressed to us, from the RS-485
    // task. Keep it short -- no allocation, no blocking, no network calls.
    using PacketHandler = std::function<void(const JandyPacket &)>;
    // Called for every packet on the bus when promiscuous mode is on.
    using SnoopHandler  = std::function<void(const JandyPacket &)>;

    void begin(uint8_t myId, bool sniffOnly, bool promiscuous);
    void onPacket(PacketHandler h) { _onPacket = h; }
    void onSnoop(SnoopHandler h)   { _onSnoop = h; }

    // Queue a single keypress to ride out on the next ACK. The panel accepts
    // only one key per ACK, so presses are serialised through a ring buffer
    // rather than overwriting each other.
    bool pressKey(uint8_t keycode);
    size_t pendingKeys() const;

    bool online() const { return _online; }
    uint32_t packetsSeen()  const { return _decoder.stats().frames; }
    uint32_t badChecksums() const { return _decoder.stats().badChecksum; }
    uint32_t resyncs()      const { return _decoder.stats().resync; }
    uint32_t lastAckMicros() const { return _lastAckMicros; }
    // Microseconds from end-of-frame to ACK transmit. Must stay well under
    // 20000; if it creeps up, something is stealing core 1.
    uint32_t ackLatencyUs() const { return _ackLatencyUs; }
    // Non-zero means the RS-485 module is not muting its receiver during
    // transmit. Harmless once suppressed, but it tells you which kind of
    // board you have.
    uint32_t echoesDropped() const { return _echoesDropped; }

private:
    static void taskTrampoline(void *arg);
    void task();
    void dispatch(const JandyPacket &p);
    bool isSelfEcho(const JandyPacket &p) const;

    JandyDecoder _decoder;

    uint8_t  _myId = 0;
    bool     _sniffOnly = false;
    bool     _promiscuous = false;
    volatile bool _online = false;

    static const size_t KEYQ_LEN = 16;
    volatile uint8_t _keyq[KEYQ_LEN];
    volatile size_t  _keyHead = 0, _keyTail = 0;
    portMUX_TYPE _keyMux = portMUX_INITIALIZER_UNLOCKED;

    volatile uint32_t _lastAckMicros = 0;
    volatile uint32_t _ackLatencyUs = 0;
    volatile uint32_t _echoesDropped = 0;
    volatile uint8_t  _lastTxKey = 0;

    PacketHandler _onPacket;
    SnoopHandler  _onSnoop;
};

extern JandyBus Bus;
