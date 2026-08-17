#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aqualinkd {

// Jandy framing and command constants, matching AqualinkD's aq_serial.h.
constexpr uint8_t NUL = 0x00;
constexpr uint8_t DLE = 0x10;
constexpr uint8_t STX = 0x02;
constexpr uint8_t ETX = 0x03;

constexpr uint8_t DEV_MASTER = 0x00;
constexpr uint8_t CMD_PROBE = 0x00;
constexpr uint8_t CMD_ACK = 0x01;
constexpr uint8_t CMD_STATUS = 0x02;
constexpr uint8_t CMD_MSG = 0x03;
constexpr uint8_t CMD_MSG_LONG = 0x04;
constexpr uint8_t CMD_PROBE_ALT = 0x05;

constexpr uint8_t ACK_NORMAL = 0x00;
constexpr uint8_t ACK_SCREEN_BUSY = 0x01;
constexpr uint8_t ACK_PAUSE = 0x03;
constexpr uint8_t ACK_ALLBUTTON = 0x80;
constexpr uint8_t ACK_ALLBUTTON_BUSY = 0x81;

constexpr size_t MAX_DATA_SIZE = 128;
constexpr size_t MAX_FRAME_SIZE = 2 * MAX_DATA_SIZE + 16;

struct Packet {
    uint8_t destination = 0;
    uint8_t command = 0;
    uint8_t data[MAX_DATA_SIZE] = {};
    size_t dataLength = 0;
};

enum class DecodeResult : uint8_t {
    None,
    PacketReady,
    BadChecksum,
    Overflow,
};

uint8_t checksum(uint8_t destination, uint8_t command,
                 const uint8_t* data, size_t dataLength);

// Encodes [NUL] DLE STX <dest> <cmd> <data> <checksum> DLE ETX [NUL],
// including DLE stuffing. Returns zero if outputCapacity is too small.
size_t encodeFrame(uint8_t destination, uint8_t command,
                   const uint8_t* data, size_t dataLength,
                   uint8_t* output, size_t outputCapacity);

size_t encodeAck(uint8_t ackType, uint8_t keyCode,
                 uint8_t* output, size_t outputCapacity);

class Decoder {
public:
    DecodeResult feed(uint8_t byte, Packet& packet);
    void reset();

private:
    enum class State : uint8_t { Idle, GotDle, Data, DataDle };

    DecodeResult finish(Packet& packet);
    bool append(uint8_t byte);

    State state_ = State::Idle;
    uint8_t frame_[MAX_DATA_SIZE + 5] = {};
    size_t length_ = 0;
};

}  // namespace aqualinkd
