#include <unity.h>
#include <string.h>
#include "jandy_codec.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static JandyDecoder dec;

// Feed a byte stream, return how many packets came out; last one in `out`.
static int feedAll(const uint8_t *buf, size_t n, JandyPacket &out) {
    int count = 0;
    JandyPacket p;
    for (size_t i = 0; i < n; i++) {
        if (dec.feed(buf[i], p)) { out = p; count++; }
    }
    return count;
}

void setUp(void)    { dec.reset(); }
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Checksum -- anchored to AqualinkD's hardcoded null-ACK packet.
// If this test fails, the framing understanding is wrong, not the code.
// ---------------------------------------------------------------------------
void test_checksum_matches_known_ack(void) {
    const uint8_t body[] = { 0x10, 0x02, 0x00, 0x01, 0x00, 0x00 };
    TEST_ASSERT_EQUAL_HEX8(0x13, jandy_checksum(body, sizeof(body)));
}

void test_build_ack_shape(void) {
    uint8_t buf[JANDY_ACK_LEN];
    size_t n = jandy_build_ack(buf, sizeof(buf), ACK_NORMAL, 0x00);
    TEST_ASSERT_EQUAL_UINT(JANDY_ACK_LEN, n);

    const uint8_t expect[] = { 0x00, 0x10, 0x02, 0x00, 0x01,
                               0x00, 0x00, 0x13, 0x10, 0x03, 0x00 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, buf, JANDY_ACK_LEN);
}

void test_build_ack_carries_keycode(void) {
    uint8_t buf[JANDY_ACK_LEN];
    jandy_build_ack(buf, sizeof(buf), ACK_NORMAL, 0x05);
    TEST_ASSERT_EQUAL_HEX8(0x05, buf[6]);
    TEST_ASSERT_EQUAL_HEX8(0x18, buf[7]);   // 0x13 + 0x05
}

void test_build_ack_rejects_small_buffer(void) {
    uint8_t buf[4];
    TEST_ASSERT_EQUAL_UINT(0, jandy_build_ack(buf, sizeof(buf), 0, 0));
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------
void test_decode_simple_status(void) {
    uint8_t frame[64];
    const uint8_t payload[] = { 0x01, 0x00, 0x00 };
    size_t n = jandy_encode(frame, sizeof(frame), 0x0A, CMD_STATUS,
                            payload, sizeof(payload));
    TEST_ASSERT_GREATER_THAN(0, n);

    JandyPacket p;
    TEST_ASSERT_EQUAL_INT(1, feedAll(frame, n, p));
    TEST_ASSERT_EQUAL_HEX8(0x0A, p.dest);
    TEST_ASSERT_EQUAL_HEX8(CMD_STATUS, p.cmd);
    TEST_ASSERT_EQUAL_UINT(3, p.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, p.data, 3);
}

void test_decode_ascii_message(void) {
    uint8_t frame[64];
    const char *text = "POOL TEMP 78";
    size_t n = jandy_encode(frame, sizeof(frame), 0x0A, CMD_MSG,
                            (const uint8_t *)text, strlen(text));
    JandyPacket p;
    TEST_ASSERT_EQUAL_INT(1, feedAll(frame, n, p));
    TEST_ASSERT_EQUAL_UINT(strlen(text), p.len);
    TEST_ASSERT_EQUAL_MEMORY(text, p.data, strlen(text));
}

// A literal 0x10 in the payload is stuffed as 0x10 0x00 on the wire and must
// come back out as a single 0x10. This is the bug that eats afternoons.
void test_decode_unstuffs_dle(void) {
    uint8_t frame[64];
    const uint8_t payload[] = { 0x01, 0x10, 0x02 };
    size_t n = jandy_encode(frame, sizeof(frame), 0x0A, CMD_STATUS,
                            payload, sizeof(payload));

    // Confirm the encoder actually stuffed it.
    bool stuffed = false;
    for (size_t i = 0; i + 1 < n; i++)
        if (frame[i] == 0x10 && frame[i + 1] == 0x00 && i > 2) stuffed = true;
    TEST_ASSERT_TRUE_MESSAGE(stuffed, "encoder failed to stuff payload DLE");

    JandyPacket p;
    TEST_ASSERT_EQUAL_INT(1, feedAll(frame, n, p));
    TEST_ASSERT_EQUAL_UINT(3, p.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, p.data, 3);
}

void test_decode_rejects_bad_checksum(void) {
    uint8_t frame[64];
    const uint8_t payload[] = { 0x01 };
    size_t n = jandy_encode(frame, sizeof(frame), 0x0A, CMD_STATUS,
                            payload, sizeof(payload));
    frame[n - 4] ^= 0xFF;               // corrupt the checksum byte

    JandyPacket p;
    TEST_ASSERT_EQUAL_INT(0, feedAll(frame, n, p));
    TEST_ASSERT_EQUAL_UINT(1, dec.stats().badChecksum);
    TEST_ASSERT_EQUAL_UINT(0, dec.stats().frames);
}

// Leading garbage must not prevent the following frame from decoding.
void test_decode_resyncs_after_noise(void) {
    uint8_t stream[128];
    size_t n = 0;
    const uint8_t noise[] = { 0xFF, 0x00, 0x55, 0x10, 0xAA, 0x03 };
    memcpy(stream, noise, sizeof(noise));
    n += sizeof(noise);

    const uint8_t payload[] = { 0x02 };
    n += jandy_encode(stream + n, sizeof(stream) - n, 0x0A, CMD_STATUS,
                      payload, sizeof(payload));

    JandyPacket p;
    TEST_ASSERT_EQUAL_INT(1, feedAll(stream, n, p));
    TEST_ASSERT_EQUAL_HEX8(0x0A, p.dest);
}

// A frame cut off mid-flight followed by a good one: the good one survives.
void test_decode_truncated_then_good(void) {
    uint8_t stream[128];
    size_t n = 0;
    const uint8_t partial[] = { 0x10, 0x02, 0x0A, 0x02, 0x11, 0x22 };
    memcpy(stream, partial, sizeof(partial));
    n += sizeof(partial);

    const uint8_t payload[] = { 0x03 };
    n += jandy_encode(stream + n, sizeof(stream) - n, 0x08, CMD_MSG,
                      payload, sizeof(payload));

    JandyPacket p;
    TEST_ASSERT_EQUAL_INT(1, feedAll(stream, n, p));
    TEST_ASSERT_EQUAL_HEX8(0x08, p.dest);
    TEST_ASSERT_EQUAL_HEX8(CMD_MSG, p.cmd);
}

void test_decode_back_to_back_frames(void) {
    uint8_t stream[128];
    size_t n = 0;
    const uint8_t a[] = { 0x01 };
    const uint8_t b[] = { 0x02 };
    n += jandy_encode(stream + n, sizeof(stream) - n, 0x0A, CMD_STATUS, a, 1);
    n += jandy_encode(stream + n, sizeof(stream) - n, 0x0B, CMD_STATUS, b, 1);

    JandyPacket p;
    TEST_ASSERT_EQUAL_INT(2, feedAll(stream, n, p));
    TEST_ASSERT_EQUAL_HEX8(0x0B, p.dest);   // last one wins
}

void test_decode_probe_has_empty_payload(void) {
    uint8_t frame[32];
    size_t n = jandy_encode(frame, sizeof(frame), 0x0A, CMD_PROBE, NULL, 0);
    JandyPacket p;
    TEST_ASSERT_EQUAL_INT(1, feedAll(frame, n, p));
    TEST_ASSERT_EQUAL_UINT(0, p.len);
    TEST_ASSERT_EQUAL_HEX8(CMD_PROBE, p.cmd);
}

// ---------------------------------------------------------------------------
int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_checksum_matches_known_ack);
    RUN_TEST(test_build_ack_shape);
    RUN_TEST(test_build_ack_carries_keycode);
    RUN_TEST(test_build_ack_rejects_small_buffer);
    RUN_TEST(test_decode_simple_status);
    RUN_TEST(test_decode_ascii_message);
    RUN_TEST(test_decode_unstuffs_dle);
    RUN_TEST(test_decode_rejects_bad_checksum);
    RUN_TEST(test_decode_resyncs_after_noise);
    RUN_TEST(test_decode_truncated_then_good);
    RUN_TEST(test_decode_back_to_back_frames);
    RUN_TEST(test_decode_probe_has_empty_payload);
    return UNITY_END();
}
