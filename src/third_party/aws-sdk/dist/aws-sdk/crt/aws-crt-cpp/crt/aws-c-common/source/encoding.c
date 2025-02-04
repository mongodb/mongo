/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/encoding.h>

#include <ctype.h>
#include <stdlib.h>

#ifdef USE_SIMD_ENCODING
size_t aws_common_private_base64_decode_sse41(const unsigned char *in, unsigned char *out, size_t len);
void aws_common_private_base64_encode_sse41(const unsigned char *in, unsigned char *out, size_t len);
bool aws_common_private_has_avx2(void);
#else
/*
 * When AVX2 compilation is unavailable, we use these stubs to fall back to the pure-C decoder.
 * Since we force aws_common_private_has_avx2 to return false, the encode and decode functions should
 * not be called - but we must provide them anyway to avoid link errors.
 */
static inline size_t aws_common_private_base64_decode_sse41(const unsigned char *in, unsigned char *out, size_t len) {
    (void)in;
    (void)out;
    (void)len;
    AWS_ASSERT(false);
    return SIZE_MAX; /* unreachable */
}
static inline void aws_common_private_base64_encode_sse41(const unsigned char *in, unsigned char *out, size_t len) {
    (void)in;
    (void)out;
    (void)len;
    AWS_ASSERT(false);
}
static inline bool aws_common_private_has_avx2(void) {
    return false;
}
#endif

static const uint8_t *HEX_CHARS = (const uint8_t *)"0123456789abcdef";

static const uint8_t BASE64_SENTINEL_VALUE = 0xff;
static const uint8_t BASE64_ENCODING_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* in this table, 0xDD is an invalid decoded value, if you have to do byte counting for any reason, there's 16 bytes
 * per row.  Reformatting is turned off to make sure this stays as 16 bytes per line. */
/* clang-format off */
static const uint8_t BASE64_DECODING_TABLE[256] = {
    64,   0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD,
    0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD,
    0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 62,   0xDD, 0xDD, 0xDD, 63,
    52,   53,   54,   55,   56,   57,   58,   59,   60,   61,   0xDD, 0xDD, 0xDD, 255,  0xDD, 0xDD,
    0xDD, 0,    1,    2,    3,    4,    5,    6,    7,    8,    9,    10,   11,   12,   13,   14,
    15,   16,   17,   18,   19,   20,   21,   22,   23,   24,   25,   0xDD, 0xDD, 0xDD, 0xDD, 0xDD,
    0xDD, 26,   27,   28,   29,   30,   31,   32,   33,   34,   35,   36,   37,   38,   39,   40,
    41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51,   0xDD, 0xDD, 0xDD, 0xDD, 0xDD,
    0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD,
    0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD,
    0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD,
    0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD,
    0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD,
    0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD,
    0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD,
    0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD};
/* clang-format on */

int aws_hex_compute_encoded_len(size_t to_encode_len, size_t *encoded_length) {
    AWS_ASSERT(encoded_length);

    size_t temp = (to_encode_len << 1) + 1;

    if (AWS_UNLIKELY(temp < to_encode_len)) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }

    *encoded_length = temp;

    return AWS_OP_SUCCESS;
}

int aws_hex_encode(const struct aws_byte_cursor *AWS_RESTRICT to_encode, struct aws_byte_buf *AWS_RESTRICT output) {
    AWS_PRECONDITION(aws_byte_cursor_is_valid(to_encode));
    AWS_PRECONDITION(aws_byte_buf_is_valid(output));

    size_t encoded_len = 0;

    if (AWS_UNLIKELY(aws_hex_compute_encoded_len(to_encode->len, &encoded_len))) {
        return AWS_OP_ERR;
    }

    if (AWS_UNLIKELY(output->capacity < encoded_len)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    size_t written = 0;
    for (size_t i = 0; i < to_encode->len; ++i) {

        output->buffer[written++] = HEX_CHARS[to_encode->ptr[i] >> 4 & 0x0f];
        output->buffer[written++] = HEX_CHARS[to_encode->ptr[i] & 0x0f];
    }

    output->buffer[written] = '\0';
    output->len = encoded_len;

    return AWS_OP_SUCCESS;
}

int aws_hex_encode_append_dynamic(
    const struct aws_byte_cursor *AWS_RESTRICT to_encode,
    struct aws_byte_buf *AWS_RESTRICT output) {
    AWS_ASSERT(to_encode->ptr);
    AWS_ASSERT(aws_byte_buf_is_valid(output));

    size_t encoded_len = 0;
    if (AWS_UNLIKELY(aws_add_size_checked(to_encode->len, to_encode->len, &encoded_len))) {
        return AWS_OP_ERR;
    }

    if (AWS_UNLIKELY(aws_byte_buf_reserve_relative(output, encoded_len))) {
        return AWS_OP_ERR;
    }

    size_t written = output->len;
    for (size_t i = 0; i < to_encode->len; ++i) {

        output->buffer[written++] = HEX_CHARS[to_encode->ptr[i] >> 4 & 0x0f];
        output->buffer[written++] = HEX_CHARS[to_encode->ptr[i] & 0x0f];
    }

    output->len += encoded_len;

    return AWS_OP_SUCCESS;
}

static int s_hex_decode_char_to_int(char character, uint8_t *int_val) {
    if (character >= 'a' && character <= 'f') {
        *int_val = (uint8_t)(10 + (character - 'a'));
        return 0;
    }

    if (character >= 'A' && character <= 'F') {
        *int_val = (uint8_t)(10 + (character - 'A'));
        return 0;
    }

    if (character >= '0' && character <= '9') {
        *int_val = (uint8_t)(character - '0');
        return 0;
    }

    return AWS_OP_ERR;
}

int aws_hex_compute_decoded_len(size_t to_decode_len, size_t *decoded_len) {
    AWS_ASSERT(decoded_len);

    size_t temp = (to_decode_len + 1);

    if (AWS_UNLIKELY(temp < to_decode_len)) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }

    *decoded_len = temp >> 1;
    return AWS_OP_SUCCESS;
}

int aws_hex_decode(const struct aws_byte_cursor *AWS_RESTRICT to_decode, struct aws_byte_buf *AWS_RESTRICT output) {
    AWS_PRECONDITION(aws_byte_cursor_is_valid(to_decode));
    AWS_PRECONDITION(aws_byte_buf_is_valid(output));

    size_t decoded_length = 0;

    if (AWS_UNLIKELY(aws_hex_compute_decoded_len(to_decode->len, &decoded_length))) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }

    if (AWS_UNLIKELY(output->capacity < decoded_length)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    size_t written = 0;
    size_t i = 0;
    uint8_t high_value = 0;
    uint8_t low_value = 0;

    /* if the buffer isn't even, prepend a 0 to the buffer. */
    if (AWS_UNLIKELY(to_decode->len & 0x01)) {
        i = 1;
        if (s_hex_decode_char_to_int((char)to_decode->ptr[0], &low_value)) {
            return aws_raise_error(AWS_ERROR_INVALID_HEX_STR);
        }

        output->buffer[written++] = low_value;
    }

    for (; i < to_decode->len; i += 2) {
        if (AWS_UNLIKELY(
                s_hex_decode_char_to_int(to_decode->ptr[i], &high_value) ||
                s_hex_decode_char_to_int(to_decode->ptr[i + 1], &low_value))) {
            return aws_raise_error(AWS_ERROR_INVALID_HEX_STR);
        }

        uint8_t value = (uint8_t)(high_value << 4);
        value |= low_value;
        output->buffer[written++] = value;
    }

    output->len = decoded_length;

    return AWS_OP_SUCCESS;
}

int aws_base64_compute_encoded_len(size_t to_encode_len, size_t *encoded_len) {
    AWS_ASSERT(encoded_len);

    size_t tmp = to_encode_len + 2;

    if (AWS_UNLIKELY(tmp < to_encode_len)) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }

    tmp /= 3;
    size_t overflow_check = tmp;
    tmp = 4 * tmp + 1; /* plus one for the NULL terminator */

    if (AWS_UNLIKELY(tmp < overflow_check)) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }

    *encoded_len = tmp;

    return AWS_OP_SUCCESS;
}

int aws_base64_compute_decoded_len(const struct aws_byte_cursor *AWS_RESTRICT to_decode, size_t *decoded_len) {
    AWS_ASSERT(to_decode);
    AWS_ASSERT(decoded_len);

    const size_t len = to_decode->len;
    const uint8_t *input = to_decode->ptr;

    if (len == 0) {
        *decoded_len = 0;
        return AWS_OP_SUCCESS;
    }

    if (AWS_UNLIKELY(len & 0x03)) {
        return aws_raise_error(AWS_ERROR_INVALID_BASE64_STR);
    }

    size_t tmp = len * 3;

    if (AWS_UNLIKELY(tmp < len)) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }

    size_t padding = 0;

    if (len >= 2 && input[len - 1] == '=' && input[len - 2] == '=') { /*last two chars are = */
        padding = 2;
    } else if (input[len - 1] == '=') { /*last char is = */
        padding = 1;
    }

    *decoded_len = (tmp / 4 - padding);
    return AWS_OP_SUCCESS;
}

int aws_base64_encode(const struct aws_byte_cursor *AWS_RESTRICT to_encode, struct aws_byte_buf *AWS_RESTRICT output) {
    AWS_ASSERT(to_encode->ptr);
    AWS_ASSERT(output->buffer);

    size_t terminated_length = 0;
    size_t encoded_length = 0;
    if (AWS_UNLIKELY(aws_base64_compute_encoded_len(to_encode->len, &terminated_length))) {
        return AWS_OP_ERR;
    }

    size_t needed_capacity = 0;
    if (AWS_UNLIKELY(aws_add_size_checked(output->len, terminated_length, &needed_capacity))) {
        return AWS_OP_ERR;
    }

    if (AWS_UNLIKELY(output->capacity < needed_capacity)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    /*
     * For convenience to standard C functions expecting a null-terminated
     * string, the output is terminated. As the encoding itself can be used in
     * various ways, however, its length should never account for that byte.
     */
    encoded_length = (terminated_length - 1);

    if (aws_common_private_has_avx2()) {
        aws_common_private_base64_encode_sse41(to_encode->ptr, output->buffer + output->len, to_encode->len);
        output->buffer[output->len + encoded_length] = 0;
        output->len += encoded_length;
        return AWS_OP_SUCCESS;
    }

    size_t buffer_length = to_encode->len;
    size_t block_count = (buffer_length + 2) / 3;
    size_t remainder_count = (buffer_length % 3);
    size_t str_index = output->len;

    for (size_t i = 0; i < to_encode->len; i += 3) {
        uint32_t block = to_encode->ptr[i];

        block <<= 8;
        if (AWS_LIKELY(i + 1 < buffer_length)) {
            block = block | to_encode->ptr[i + 1];
        }

        block <<= 8;
        if (AWS_LIKELY(i + 2 < to_encode->len)) {
            block = block | to_encode->ptr[i + 2];
        }

        output->buffer[str_index++] = BASE64_ENCODING_TABLE[(block >> 18) & 0x3F];
        output->buffer[str_index++] = BASE64_ENCODING_TABLE[(block >> 12) & 0x3F];
        output->buffer[str_index++] = BASE64_ENCODING_TABLE[(block >> 6) & 0x3F];
        output->buffer[str_index++] = BASE64_ENCODING_TABLE[block & 0x3F];
    }

    if (remainder_count > 0) {
        output->buffer[output->len + block_count * 4 - 1] = '=';
        if (remainder_count == 1) {
            output->buffer[output->len + block_count * 4 - 2] = '=';
        }
    }

    /* it's a string add the null terminator. */
    output->buffer[output->len + encoded_length] = 0;

    output->len += encoded_length;

    return AWS_OP_SUCCESS;
}

static inline int s_base64_get_decoded_value(unsigned char to_decode, uint8_t *value, int8_t allow_sentinel) {

    uint8_t decode_value = BASE64_DECODING_TABLE[(size_t)to_decode];
    if (decode_value != 0xDD && (decode_value != BASE64_SENTINEL_VALUE || allow_sentinel)) {
        *value = decode_value;
        return AWS_OP_SUCCESS;
    }

    return AWS_OP_ERR;
}

int aws_base64_decode(const struct aws_byte_cursor *AWS_RESTRICT to_decode, struct aws_byte_buf *AWS_RESTRICT output) {
    size_t decoded_length = 0;

    if (AWS_UNLIKELY(aws_base64_compute_decoded_len(to_decode, &decoded_length))) {
        return AWS_OP_ERR;
    }

    if (output->capacity < decoded_length) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    if (aws_common_private_has_avx2()) {
        size_t result = aws_common_private_base64_decode_sse41(to_decode->ptr, output->buffer, to_decode->len);
        if (result == SIZE_MAX) {
            return aws_raise_error(AWS_ERROR_INVALID_BASE64_STR);
        }

        output->len = result;
        return AWS_OP_SUCCESS;
    }

    int64_t block_count = (int64_t)to_decode->len / 4;
    size_t string_index = 0;
    uint8_t value1 = 0, value2 = 0, value3 = 0, value4 = 0;
    int64_t buffer_index = 0;

    for (int64_t i = 0; i < block_count - 1; ++i) {
        if (AWS_UNLIKELY(
                s_base64_get_decoded_value(to_decode->ptr[string_index++], &value1, 0) ||
                s_base64_get_decoded_value(to_decode->ptr[string_index++], &value2, 0) ||
                s_base64_get_decoded_value(to_decode->ptr[string_index++], &value3, 0) ||
                s_base64_get_decoded_value(to_decode->ptr[string_index++], &value4, 0))) {
            return aws_raise_error(AWS_ERROR_INVALID_BASE64_STR);
        }

        buffer_index = i * 3;
        output->buffer[buffer_index++] = (uint8_t)((value1 << 2) | ((value2 >> 4) & 0x03));
        output->buffer[buffer_index++] = (uint8_t)(((value2 << 4) & 0xF0) | ((value3 >> 2) & 0x0F));
        output->buffer[buffer_index] = (uint8_t)((value3 & 0x03) << 6 | value4);
    }

    buffer_index = (block_count - 1) * 3;

    if (buffer_index >= 0) {
        if (s_base64_get_decoded_value(to_decode->ptr[string_index++], &value1, 0) ||
            s_base64_get_decoded_value(to_decode->ptr[string_index++], &value2, 0) ||
            s_base64_get_decoded_value(to_decode->ptr[string_index++], &value3, 1) ||
            s_base64_get_decoded_value(to_decode->ptr[string_index], &value4, 1)) {
            return aws_raise_error(AWS_ERROR_INVALID_BASE64_STR);
        }

        output->buffer[buffer_index++] = (uint8_t)((value1 << 2) | ((value2 >> 4) & 0x03));

        if (value3 != BASE64_SENTINEL_VALUE) {
            output->buffer[buffer_index++] = (uint8_t)(((value2 << 4) & 0xF0) | ((value3 >> 2) & 0x0F));
            if (value4 != BASE64_SENTINEL_VALUE) {
                output->buffer[buffer_index] = (uint8_t)((value3 & 0x03) << 6 | value4);
            }
        }
    }
    output->len = decoded_length;
    return AWS_OP_SUCCESS;
}

struct aws_utf8_decoder {
    struct aws_allocator *alloc;
    /* Value of current codepoint, updated as we read each byte */
    uint32_t codepoint;
    /* Minimum value that current codepoint is allowed to end up with
     * (i.e. text cannot use 2 bytes to encode what would have fit in 1 byte) */
    uint32_t min;
    /* Number of bytes remaining the current codepoint */
    uint8_t remaining;
    /* Custom callback */
    int (*on_codepoint)(uint32_t codepoint, void *user_data);
    /* user_data for on_codepoint */
    void *user_data;
};

struct aws_utf8_decoder *aws_utf8_decoder_new(
    struct aws_allocator *allocator,
    const struct aws_utf8_decoder_options *options) {

    struct aws_utf8_decoder *decoder = aws_mem_calloc(allocator, 1, sizeof(struct aws_utf8_decoder));
    decoder->alloc = allocator;
    if (options) {
        decoder->on_codepoint = options->on_codepoint;
        decoder->user_data = options->user_data;
    }
    return decoder;
}

void aws_utf8_decoder_destroy(struct aws_utf8_decoder *decoder) {
    if (decoder) {
        aws_mem_release(decoder->alloc, decoder);
    }
}

void aws_utf8_decoder_reset(struct aws_utf8_decoder *decoder) {
    decoder->codepoint = 0;
    decoder->min = 0;
    decoder->remaining = 0;
}

/* Why yes, this could be optimized. */
int aws_utf8_decoder_update(struct aws_utf8_decoder *decoder, struct aws_byte_cursor bytes) {

    /* We're respecting RFC-3629, which uses 1 to 4 byte sequences (never 5 or 6) */
    for (size_t i = 0; i < bytes.len; ++i) {
        uint8_t byte = bytes.ptr[i];

        if (decoder->remaining == 0) {
            /* Check first byte of the codepoint to determine how many more bytes remain */
            if ((byte & 0x80) == 0x00) {
                /* 1 byte codepoints start with 0xxxxxxx */
                decoder->remaining = 0;
                decoder->codepoint = byte;
                decoder->min = 0;
            } else if ((byte & 0xE0) == 0xC0) {
                /* 2 byte codepoints start with 110xxxxx */
                decoder->remaining = 1;
                decoder->codepoint = byte & 0x1F;
                decoder->min = 0x80;
            } else if ((byte & 0xF0) == 0xE0) {
                /* 3 byte codepoints start with 1110xxxx */
                decoder->remaining = 2;
                decoder->codepoint = byte & 0x0F;
                decoder->min = 0x800;
            } else if ((byte & 0xF8) == 0xF0) {
                /* 4 byte codepoints start with 11110xxx */
                decoder->remaining = 3;
                decoder->codepoint = byte & 0x07;
                decoder->min = 0x10000;
            } else {
                return aws_raise_error(AWS_ERROR_INVALID_UTF8);
            }
        } else {
            /* This is not the first byte of a codepoint.
             * Ensure it starts with 10xxxxxx*/
            if ((byte & 0xC0) != 0x80) {
                return aws_raise_error(AWS_ERROR_INVALID_UTF8);
            }

            /* Insert the 6 newly decoded bits:
             * shifting left anything we've already decoded, and insert the new bits to the right */
            decoder->codepoint = (decoder->codepoint << 6) | (byte & 0x3F);

            /* If we've decoded the whole codepoint, check it for validity
             * (don't need to do these particular checks on 1 byte codepoints) */
            if (--decoder->remaining == 0) {
                /* Check that it's not "overlong" (encoded using more bytes than necessary) */
                if (decoder->codepoint < decoder->min) {
                    return aws_raise_error(AWS_ERROR_INVALID_UTF8);
                }

                /* UTF-8 prohibits encoding character numbers between U+D800 and U+DFFF,
                 * which are reserved for use with the UTF-16 encoding form (as
                 * surrogate pairs) and do not directly represent characters */
                if (decoder->codepoint >= 0xD800 && decoder->codepoint <= 0xDFFF) {
                    return aws_raise_error(AWS_ERROR_INVALID_UTF8);
                }
            }
        }

        /* Invoke user's on_codepoint callback */
        if (decoder->on_codepoint && decoder->remaining == 0) {
            if (decoder->on_codepoint(decoder->codepoint, decoder->user_data)) {
                return AWS_OP_ERR;
            }
        }
    }

    return AWS_OP_SUCCESS;
}

int aws_utf8_decoder_finalize(struct aws_utf8_decoder *decoder) {
    bool valid = decoder->remaining == 0;
    aws_utf8_decoder_reset(decoder);
    if (AWS_LIKELY(valid)) {
        return AWS_OP_SUCCESS;
    }
    return aws_raise_error(AWS_ERROR_INVALID_UTF8);
}

int aws_decode_utf8(struct aws_byte_cursor bytes, const struct aws_utf8_decoder_options *options) {
    struct aws_utf8_decoder decoder = {
        .on_codepoint = options ? options->on_codepoint : NULL,
        .user_data = options ? options->user_data : NULL,
    };

    if (aws_utf8_decoder_update(&decoder, bytes)) {
        return AWS_OP_ERR;
    }

    if (aws_utf8_decoder_finalize(&decoder)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}
