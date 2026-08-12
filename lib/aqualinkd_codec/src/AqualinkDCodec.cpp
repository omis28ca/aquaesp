#include "AqualinkDCodec.h"

#include <string.h>

namespace aqualinkd {
namespace {

bool appendStuffed(uint8_t byte, uint8_t* output, size_t outputCapacity,
                   size_t& outputLength) {
    const size_t required = byte == DLE ? 2 : 1;
    if (outputLength + required > outputCapacity) {
        return false;
    }
    output[outputLength++] = byte;
    if (byte == DLE) {
        output[outputLength++] = NUL;
    }
    return true;
}

}  // namespace

uint8_t checksum(uint8_t destination, uint8_t command,
                 const uint8_t* data, size_t dataLength) {
    uint16_t sum = static_cast<uint16_t>(DLE) + STX + destination + command;
    for (size_t i = 0; i < dataLength; ++i) {
        sum += data[i];
    }
    return static_cast<uint8_t>(sum & 0xFF);
}

size_t encodeFrame(uint8_t destination, uint8_t command,
                   const uint8_t* data, size_t dataLength,
                   uint8_t* output, size_t outputCapacity) {
    if (output == nullptr || dataLength > MAX_DATA_SIZE ||
        (dataLength > 0 && data == nullptr) || outputCapacity < 6) {
        return 0;
    }

    size_t length = 0;
    output[length++] = NUL;
    output[length++] = DLE;
    output[length++] = STX;

    if (!appendStuffed(destination, output, outputCapacity, length) ||
        !appendStuffed(command, output, outputCapacity, length)) {
        return 0;
    }
    for (size_t i = 0; i < dataLength; ++i) {
        if (!appendStuffed(data[i], output, outputCapacity, length)) {
            return 0;
        }
    }
    if (!appendStuffed(checksum(destination, command, data, dataLength),
                       output, outputCapacity, length) ||
        length + 3 > outputCapacity) {
        return 0;
    }

    output[length++] = DLE;
    output[length++] = ETX;
    output[length++] = NUL;
    return length;
}

size_t encodeAck(uint8_t ackType, uint8_t keyCode,
                 uint8_t* output, size_t outputCapacity) {
    const uint8_t data[] = {ackType, keyCode};
    return encodeFrame(DEV_MASTER, CMD_ACK, data, sizeof(data),
                       output, outputCapacity);
}

void Decoder::reset() {
    state_ = State::Idle;
    length_ = 0;
}

bool Decoder::append(uint8_t byte) {
    if (length_ >= sizeof(frame_)) {
        reset();
        return false;
    }
    frame_[length_++] = byte;
    return true;
}

DecodeResult Decoder::finish(Packet& packet) {
    state_ = State::Idle;
    if (length_ < 5) {
        length_ = 0;
        return DecodeResult::BadChecksum;
    }

    const uint8_t receivedChecksum = frame_[length_ - 1];
    uint16_t sum = 0;
    for (size_t i = 0; i + 1 < length_; ++i) {
        sum += frame_[i];
    }
    if (static_cast<uint8_t>(sum & 0xFF) != receivedChecksum) {
        length_ = 0;
        return DecodeResult::BadChecksum;
    }

    packet.destination = frame_[2];
    packet.command = frame_[3];
    packet.dataLength = length_ - 5;
    if (packet.dataLength > 0) {
        memcpy(packet.data, &frame_[4], packet.dataLength);
    }
    length_ = 0;
    return DecodeResult::PacketReady;
}

DecodeResult Decoder::feed(uint8_t byte, Packet& packet) {
    switch (state_) {
        case State::Idle:
            if (byte == DLE) {
                state_ = State::GotDle;
            }
            break;

        case State::GotDle:
            if (byte == STX) {
                length_ = 0;
                append(DLE);
                append(STX);
                state_ = State::Data;
            } else if (byte != DLE) {
                state_ = State::Idle;
            }
            break;

        case State::Data:
            if (byte == DLE) {
                state_ = State::DataDle;
            } else if (!append(byte)) {
                return DecodeResult::Overflow;
            }
            break;

        case State::DataDle:
            if (byte == ETX) {
                return finish(packet);
            }
            if (byte == NUL) {
                if (!append(DLE)) {
                    return DecodeResult::Overflow;
                }
                state_ = State::Data;
            } else if (byte == STX) {
                length_ = 0;
                append(DLE);
                append(STX);
                state_ = State::Data;
            } else {
                if (!append(DLE) || !append(byte)) {
                    return DecodeResult::Overflow;
                }
                state_ = State::Data;
            }
            break;
    }
    return DecodeResult::None;
}

}  // namespace aqualinkd
