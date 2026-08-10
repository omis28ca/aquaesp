#include "jandy_serial.h"
#include "config.h"
#include <driver/uart.h>

JandyBus Bus;

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void JandyBus::begin(uint8_t myId, bool sniffOnly, bool promiscuous) {
    _myId        = myId;
    _sniffOnly   = sniffOnly;
    _promiscuous = promiscuous;
    _decoder.reset();

    uart_config_t cfg = {};
    cfg.baud_rate  = JANDY_BAUD;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_APB;

    uart_driver_install(JANDY_UART_NUM, 512, 512, 0, NULL, 0);
    uart_param_config(JANDY_UART_NUM, &cfg);

#if JANDY_PIN_DE >= 0
    // DE is driven off the RTS pin. In UART_MODE_RS485_HALF_DUPLEX the
    // hardware raises it before the start bit and drops it after the stop bit
    // of the final byte, with no software involvement -- this is what keeps
    // the turnaround deterministic under WiFi load. Preferred.
    uart_set_pin(JANDY_UART_NUM, JANDY_PIN_TX, JANDY_PIN_RX,
                 JANDY_PIN_DE, UART_PIN_NO_CHANGE);
    uart_set_mode(JANDY_UART_NUM, UART_MODE_RS485_HALF_DUPLEX);
#else
    // Auto-direction module: the board flips direction by sensing the start
    // bit, so we must NOT put the UART in RS485 mode -- there is no DE line
    // for it to drive, and the peripheral's collision detection would only
    // produce false positives.
    //
    // The trade-off is that turnaround now depends on the module's own
    // release timing rather than on the UART. At 9600 baud that is a small
    // fraction of the 60 ms budget, but it is no longer deterministic, so
    // keep an eye on ack_latency_us in /api/state.
    uart_set_pin(JANDY_UART_NUM, JANDY_PIN_TX, JANDY_PIN_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_set_mode(JANDY_UART_NUM, UART_MODE_UART);
#endif
    uart_set_rx_timeout(JANDY_UART_NUM, 2);

    xTaskCreatePinnedToCore(taskTrampoline, "jandy485", 4096, this,
                            configMAX_PRIORITIES - 4, NULL, 1);
}

void JandyBus::taskTrampoline(void *arg) {
    static_cast<JandyBus *>(arg)->task();
}

void JandyBus::task() {
    uint8_t buf[128];
    JandyPacket p;
    for (;;) {
        int n = uart_read_bytes(JANDY_UART_NUM, buf, sizeof(buf),
                                pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) {
            if (_decoder.feed(buf[i], p)) {
                p.micros_received = micros();
                dispatch(p);
            }
        }
    }
}

// ---------------------------------------------------------------------------
void JandyBus::dispatch(const JandyPacket &p) {
    // Auto-direction modules may not mute the receiver while transmitting, so
    // our own ACK can come straight back at us. Drop it before it reaches the
    // snoop log, where it would otherwise show up as a phantom keypress from
    // another device and confuse learn mode.
    if (isSelfEcho(p)) { _echoesDropped++; return; }

    if (_promiscuous && _onSnoop) _onSnoop(p);
    if (p.dest != _myId) return;

    // Ordering matters: the ACK is the time-critical part and the handler may
    // walk 16 LED entries and a text line. Answer the panel, then parse.
    if (!_sniffOnly) {
        uint8_t key = 0;
        portENTER_CRITICAL(&_keyMux);
        if (_keyHead != _keyTail) {
            key = _keyq[_keyTail];
            _keyTail = (_keyTail + 1) % KEYQ_LEN;
        }
        portEXIT_CRITICAL(&_keyMux);

        uint8_t ack[JANDY_ACK_LEN];
        size_t n = jandy_build_ack(ack, sizeof(ack), ACK_NORMAL, key);
        uart_write_bytes(JANDY_UART_NUM, (const char *)ack, n);

        _lastAckMicros = micros();
        _ackLatencyUs  = _lastAckMicros - p.micros_received;
        _lastTxKey     = key;
        _online = true;
    }

    if (_onPacket) _onPacket(p);
}

// ---------------------------------------------------------------------------
// Key queue -- written from the network core, drained by the RS-485 task.
// ---------------------------------------------------------------------------
bool JandyBus::pressKey(uint8_t keycode) {
    bool ok = false;
    portENTER_CRITICAL(&_keyMux);
    size_t next = (_keyHead + 1) % KEYQ_LEN;
    if (next != _keyTail) {
        _keyq[_keyHead] = keycode;
        _keyHead = next;
        ok = true;
    }
    portEXIT_CRITICAL(&_keyMux);
    return ok;
}

size_t JandyBus::pendingKeys() const {
    return (_keyHead + KEYQ_LEN - _keyTail) % KEYQ_LEN;
}

// ---------------------------------------------------------------------------
// Only ACK frames addressed to the master can be ours, and only within a short
// window of us transmitting. The real wall keypad ACKs immediately after the
// panel polls IT -- a moment when we are silent -- so this test does not
// swallow the keypresses that learn mode depends on.
bool JandyBus::isSelfEcho(const JandyPacket &p) const {
    if (_sniffOnly) return false;              // we never transmit
    if (p.dest != DEV_MASTER || p.cmd != CMD_ACK) return false;
    if (_lastAckMicros == 0) return false;
    if ((uint32_t)(p.micros_received - _lastAckMicros) > JANDY_ECHO_WINDOW_US)
        return false;
    // Final check: the payload has to be the ACK we actually sent.
    return (p.len >= 2 && p.data[1] == _lastTxKey);
}
