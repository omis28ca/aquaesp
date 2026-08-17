#include "AqualinkD.h"

#include <esp_timer.h>

namespace aqualinkd {

JandyBus Bus;

JandyBus::~JandyBus() {
    end();
}

bool JandyBus::begin(const BusConfig& config, PacketHandler handler,
                     void* handlerContext) {
    if (running() || config.deviceId < 0x08 || config.deviceId > 0x0B) {
        return false;
    }

    config_ = config;
    handler_ = handler;
    handlerContext_ = handlerContext;

    const uart_config_t uartConfig = {
        .baud_rate = static_cast<int>(config_.baud),
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_APB,
    };

    if (uart_param_config(config_.uartNumber, &uartConfig) != ESP_OK ||
        uart_set_pin(config_.uartNumber, config_.txPin, config_.rxPin,
                     config_.dePin >= 0 ? config_.dePin : UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE) != ESP_OK ||
        uart_driver_install(config_.uartNumber, 1024, 512, 0, nullptr, 0) != ESP_OK) {
        return false;
    }

    // Keep both transceiver paths: hardware-controlled DE is deterministic;
    // auto-direction adapters stay in ordinary UART mode and use echo filtering.
    if (config_.dePin >= 0) {
        if (uart_set_mode(config_.uartNumber, UART_MODE_RS485_HALF_DUPLEX) != ESP_OK) {
            uart_driver_delete(config_.uartNumber);
            return false;
        }
    } else if (uart_set_mode(config_.uartNumber, UART_MODE_UART) != ESP_OK) {
        uart_driver_delete(config_.uartNumber);
        return false;
    }

    keyQueue_ = xQueueCreate(16, sizeof(uint8_t));
    if (keyQueue_ == nullptr) {
        uart_driver_delete(config_.uartNumber);
        return false;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(
        taskEntry, "jandy-rs485", 4096, this, configMAX_PRIORITIES - 4,
        &task_, 1);
    if (created != pdPASS) {
        vQueueDelete(keyQueue_);
        keyQueue_ = nullptr;
        uart_driver_delete(config_.uartNumber);
        task_ = nullptr;
        return false;
    }
    return true;
}

void JandyBus::end() {
    if (task_ != nullptr) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    if (keyQueue_ != nullptr) {
        vQueueDelete(keyQueue_);
        keyQueue_ = nullptr;
    }
    if (config_.baud != 0) {
        uart_driver_delete(config_.uartNumber);
        config_.baud = 0;
    }
    decoder_.reset();
}

bool JandyBus::queueKey(uint8_t keyCode) {
    return keyQueue_ != nullptr &&
           xQueueSend(keyQueue_, &keyCode, 0) == pdTRUE;
}

bool JandyBus::online() const {
    constexpr uint64_t ONLINE_TIMEOUT_US = 5000000;
    return running() && lastAddressedPacketUs_ != 0 &&
           static_cast<uint64_t>(esp_timer_get_time()) - lastAddressedPacketUs_ <
               ONLINE_TIMEOUT_US;
}

size_t JandyBus::pendingKeys() const {
    return keyQueue_ == nullptr ? 0 : uxQueueMessagesWaiting(keyQueue_);
}

BusStats JandyBus::stats() const {
    return {
        packetsReceived_, checksumErrors_, framesOverflowed_,
        acknowledgementsSent_, echoesDropped_, ackLatencyUs_,
    };
}

void JandyBus::taskEntry(void* context) {
    static_cast<JandyBus*>(context)->run();
}

bool JandyBus::isSelfEcho(const Packet& packet, uint64_t receivedAtUs) const {
    return config_.dePin < 0 && receivedAtUs < echoUntilUs_ &&
           packet.destination == DEV_MASTER && packet.command == CMD_ACK &&
           packet.dataLength >= 2 && packet.data[0] == lastAckType_ &&
           packet.data[1] == lastKeyCode_;
}

void JandyBus::run() {
    uint8_t input = 0;
    Packet packet;

    for (;;) {
        if (uart_read_bytes(config_.uartNumber, &input, 1, portMAX_DELAY) != 1) {
            continue;
        }

        const uint64_t nowUs = esp_timer_get_time();
        const DecodeResult result = decoder_.feed(input, packet);
        if (result == DecodeResult::PacketReady) {
            if (isSelfEcho(packet, nowUs)) {
                ++echoesDropped_;
                continue;
            }
            ++packetsReceived_;
            dispatch(packet, nowUs);
        } else if (result == DecodeResult::BadChecksum) {
            ++checksumErrors_;
        } else if (result == DecodeResult::Overflow) {
            ++framesOverflowed_;
        }
    }
}

void JandyBus::dispatch(const Packet& packet, uint64_t receivedAtUs) {
    const bool addressedToUs = packet.destination == config_.deviceId;
    if (addressedToUs) {
        lastAddressedPacketUs_ = receivedAtUs;
    }

    // A late ACK is worse than a missed ACK. Transmit before invoking any
    // application callback, which may only queue work for another task.
    if (addressedToUs && !config_.sniffOnly) {
        uint8_t keyCode = 0;
        if (packet.command == CMD_STATUS && keyQueue_ != nullptr) {
            xQueueReceive(keyQueue_, &keyCode, 0);
        }
        const uint8_t ackType =
            packet.command == CMD_MSG_LONG ? ACK_ALLBUTTON_BUSY : ACK_ALLBUTTON;
        sendAck(ackType, keyCode, receivedAtUs);
    }

    if (handler_ != nullptr && (addressedToUs || config_.promiscuous)) {
        handler_(packet, handlerContext_);
    }
}

bool JandyBus::sendAck(uint8_t ackType, uint8_t keyCode,
                       uint64_t receivedAtUs) {
    uint8_t frame[MAX_FRAME_SIZE];
    const size_t length = encodeAck(ackType, keyCode, frame, sizeof(frame));
    if (length == 0) {
        return false;
    }

    const int written = uart_write_bytes(config_.uartNumber, frame, length);
    if (written != static_cast<int>(length)) {
        return false;
    }

    ++acknowledgementsSent_;
    ackLatencyUs_ = static_cast<uint32_t>(esp_timer_get_time() - receivedAtUs);
    if (config_.dePin < 0) {
        lastAckType_ = ackType;
        lastKeyCode_ = keyCode;
        echoUntilUs_ = esp_timer_get_time() + config_.echoWindowUs;
    }
    return true;
}

}  // namespace aqualinkd
