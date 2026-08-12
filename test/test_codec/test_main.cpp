#include <unity.h>

#include <AqualinkDCodec.h>

using namespace aqualinkd;

void test_checksum_matches_known_ack() {
    const uint8_t data[] = {0x00, 0x00};
    TEST_ASSERT_EQUAL_HEX8(0x13, checksum(DEV_MASTER, CMD_ACK, data, sizeof(data)));
}

void test_known_null_ack_encoding() {
    uint8_t encoded[MAX_FRAME_SIZE] = {};
    const size_t length = encodeAck(ACK_NORMAL, 0x00, encoded, sizeof(encoded));
    const uint8_t expected[] = {
        NUL, DLE, STX, 0x00, CMD_ACK, 0x00, 0x00, 0x13, DLE, ETX, NUL,
    };
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, encoded, sizeof(expected));
}

void test_round_trip_with_dle_stuffing() {
    const uint8_t data[] = {0x44, DLE, 0x55};
    uint8_t encoded[MAX_FRAME_SIZE] = {};
    const size_t length = encodeFrame(0x0A, CMD_MSG, data, sizeof(data),
                                      encoded, sizeof(encoded));
    TEST_ASSERT_GREATER_THAN(0, length);

    Decoder decoder;
    Packet packet;
    DecodeResult result = DecodeResult::None;
    for (size_t i = 0; i < length; ++i) {
        const DecodeResult current = decoder.feed(encoded[i], packet);
        if (current != DecodeResult::None) {
            result = current;
        }
    }

    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::PacketReady),
                      static_cast<int>(result));
    TEST_ASSERT_EQUAL_HEX8(0x0A, packet.destination);
    TEST_ASSERT_EQUAL_HEX8(CMD_MSG, packet.command);
    TEST_ASSERT_EQUAL_UINT32(sizeof(data), packet.dataLength);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, packet.data, sizeof(data));
}

void test_bad_checksum_is_rejected() {
    uint8_t encoded[MAX_FRAME_SIZE] = {};
    const uint8_t data[] = {0x01};
    const size_t length = encodeFrame(0x0A, CMD_STATUS, data, sizeof(data),
                                      encoded, sizeof(encoded));
    encoded[4] ^= 0x01;

    Decoder decoder;
    Packet packet;
    DecodeResult result = DecodeResult::None;
    for (size_t i = 0; i < length; ++i) {
        const DecodeResult current = decoder.feed(encoded[i], packet);
        if (current != DecodeResult::None) {
            result = current;
        }
    }
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::BadChecksum),
                      static_cast<int>(result));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_checksum_matches_known_ack);
    RUN_TEST(test_known_null_ack_encoding);
    RUN_TEST(test_round_trip_with_dle_stuffing);
    RUN_TEST(test_bad_checksum_is_rejected);
    return UNITY_END();
}
