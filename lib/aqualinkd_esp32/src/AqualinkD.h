#pragma once

#include <Arduino.h>
#include <AqualinkDCodec.h>
#include <driver/uart.h>

namespace aqualinkd {

struct BusConfig {
    uart_port_t uartNumber;
    int rxPin;
    int txPin;
    int dePin;
    uint32_t baud;
    uint8_t deviceId;
    bool sniffOnly;
    bool promiscuous;
    uint32_t echoWindowUs;
};

struct BusStats {
    uint32_t packetsReceived;
    uint32_t checksumErrors;
    uint32_t framesOverflowed;
    uint32_t acknowledgementsSent;
    uint32_t echoesDropped;
    uint32_t acknowledgementLatencyUs;
};

using PacketHandler = void (*)(const Packet& packet, void* context);

class JandyBus {
public:
    JandyBus() = default;
    ~JandyBus();

    JandyBus(const JandyBus&) = delete;
    JandyBus& operator=(const JandyBus&) = delete;

    bool begin(const BusConfig& config, PacketHandler handler = nullptr,
               void* handlerContext = nullptr);
    void end();
    bool queueKey(uint8_t keyCode);
    bool running() const { return task_ != nullptr; }
    bool online() const;
    size_t pendingKeys() const;
    BusStats stats() const;
    uint32_t ackLatencyUs() const { return ackLatencyUs_; }

private:
    static void taskEntry(void* context);
    void run();
    void dispatch(const Packet& packet, uint64_t receivedAtUs);
    bool sendAck(uint8_t keyCode, uint64_t receivedAtUs);
    bool isSelfEcho(const Packet& packet, uint64_t receivedAtUs) const;

    BusConfig config_ = {};
    PacketHandler handler_ = nullptr;
    void* handlerContext_ = nullptr;
    TaskHandle_t task_ = nullptr;
    QueueHandle_t keyQueue_ = nullptr;
    Decoder decoder_;

    volatile uint32_t packetsReceived_ = 0;
    volatile uint32_t checksumErrors_ = 0;
    volatile uint32_t framesOverflowed_ = 0;
    volatile uint32_t acknowledgementsSent_ = 0;
    volatile uint32_t echoesDropped_ = 0;
    volatile uint32_t ackLatencyUs_ = 0;
    volatile uint64_t echoUntilUs_ = 0;
    volatile uint64_t lastAddressedPacketUs_ = 0;
    volatile uint8_t lastKeyCode_ = 0;
};

extern JandyBus Bus;

}  // namespace aqualinkd
