#ifndef AWS_COMMON_ENCODING_INL
#define AWS_COMMON_ENCODING_INL

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/byte_buf.h>
#include <aws/common/byte_order.h>
#include <aws/common/common.h>
#include <aws/common/encoding.h>

AWS_EXTERN_C_BEGIN

/* Add a 64 bit unsigned integer to the buffer, ensuring network - byte order
 * Assumes the buffer size is at least 8 bytes.
 */
AWS_STATIC_IMPL void aws_write_u64(uint64_t value, uint8_t *buffer) {
    value = aws_hton64(value);

    memcpy((void *)buffer, &value, sizeof(value));
}

/*
 * Extracts a 64 bit unsigned integer from buffer. Ensures conversion from
 * network byte order to host byte order. Assumes buffer size is at least 8
 * bytes.
 */
AWS_STATIC_IMPL uint64_t aws_read_u64(const uint8_t *buffer) {
    uint64_t value = 0;
    memcpy((void *)&value, (void *)buffer, sizeof(value));

    return aws_ntoh64(value);
}

/* Add a 32 bit unsigned integer to the buffer, ensuring network - byte order
 * Assumes the buffer size is at least 4 bytes.
 */
AWS_STATIC_IMPL void aws_write_u32(uint32_t value, uint8_t *buffer) {
    value = aws_hton32(value);

    memcpy((void *)buffer, (void *)&value, sizeof(value));
}

/*
 * Extracts a 32 bit unsigned integer from buffer. Ensures conversion from
 * network byte order to host byte order. Assumes the buffer size is at least 4
 * bytes.
 */
AWS_STATIC_IMPL uint32_t aws_read_u32(const uint8_t *buffer) {
    uint32_t value = 0;
    memcpy((void *)&value, (void *)buffer, sizeof(value));

    return aws_ntoh32(value);
}

/* Add a 24 bit unsigned integer to the buffer, ensuring network - byte order
 * return the new position in the buffer for the next operation.
 * Note, since this uses uint32_t for storage, the 3 least significant bytes
 * will be used. Assumes buffer is at least 3 bytes long.
 */
AWS_STATIC_IMPL void aws_write_u24(uint32_t value, uint8_t *buffer) {
    value = aws_hton32(value);
    memcpy((void *)buffer, (void *)((uint8_t *)&value + 1), sizeof(value) - 1);
}

/*
 * Extracts a 24 bit unsigned integer from buffer. Ensures conversion from
 * network byte order to host byte order. Assumes buffer is at least 3 bytes
 * long.
 */
AWS_STATIC_IMPL uint32_t aws_read_u24(const uint8_t *buffer) {
    uint32_t value = 0;
    memcpy((void *)((uint8_t *)&value + 1), (void *)buffer, sizeof(value) - 1);

    return aws_ntoh32(value);
}

/* Add a 16 bit unsigned integer to the buffer, ensuring network-byte order
 * return the new position in the buffer for the next operation.
 * Assumes buffer is at least 2 bytes long.
 */
AWS_STATIC_IMPL void aws_write_u16(uint16_t value, uint8_t *buffer) {
    value = aws_hton16(value);

    memcpy((void *)buffer, (void *)&value, sizeof(value));
}

/*
 * Extracts a 16 bit unsigned integer from buffer. Ensures conversion from
 * network byte order to host byte order. Assumes buffer is at least 2 bytes
 * long.
 */
AWS_STATIC_IMPL uint16_t aws_read_u16(const uint8_t *buffer) {
    uint16_t value = 0;
    memcpy((void *)&value, (void *)buffer, sizeof(value));

    return aws_ntoh16(value);
}

/* Reference: https://unicodebook.readthedocs.io/guess_encoding.html */
AWS_STATIC_IMPL enum aws_text_encoding aws_text_detect_encoding(const uint8_t *bytes, size_t size) {
    static const char *UTF_8_BOM = "\xEF\xBB\xBF";
    static const char *UTF_16_BE_BOM = "\xFE\xFF";
    static const char *UTF_16_LE_BOM = "\xFF\xFE";
    static const char *UTF_32_BE_BOM = "\x00\x00\xFE\xFF";
    static const char *UTF_32_LE_BOM = "\xFF\xFE\x00\x00";

    if (size >= 3) {
        if (memcmp(bytes, UTF_8_BOM, 3) == 0)
            return AWS_TEXT_UTF8;
    }
    if (size >= 4) {
        if (memcmp(bytes, UTF_32_LE_BOM, 4) == 0)
            return AWS_TEXT_UTF32;
        if (memcmp(bytes, UTF_32_BE_BOM, 4) == 0)
            return AWS_TEXT_UTF32;
    }
    if (size >= 2) {
        if (memcmp(bytes, UTF_16_LE_BOM, 2) == 0)
            return AWS_TEXT_UTF16;
        if (memcmp(bytes, UTF_16_BE_BOM, 2) == 0)
            return AWS_TEXT_UTF16;
    }
    size_t idx = 0;
    for (; idx < size; ++idx) {
        if (bytes[idx] & 0x80) {
            return AWS_TEXT_UNKNOWN;
        }
    }
    return AWS_TEXT_ASCII;
}

AWS_STATIC_IMPL bool aws_text_is_utf8(const uint8_t *bytes, size_t size) {
    enum aws_text_encoding encoding = aws_text_detect_encoding(bytes, size);
    return encoding == AWS_TEXT_UTF8 || encoding == AWS_TEXT_ASCII;
}

AWS_EXTERN_C_END

#endif /*  AWS_COMMON_ENCODING_INL */
