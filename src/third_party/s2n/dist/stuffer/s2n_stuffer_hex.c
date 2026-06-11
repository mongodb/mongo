/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "utils/s2n_safety.h"

static const uint8_t value_to_hex[16] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'
};

static const uint8_t hex_to_value[] = {
    /* clang-format off */
    ['0'] = 0,  ['1'] = 1,  ['2'] = 2,  ['3'] = 3,  ['4'] = 4,
    ['5'] = 5,  ['6'] = 6,  ['7'] = 7,  ['8'] = 8,  ['9'] = 9,
    ['a'] = 10, ['b'] = 11, ['c'] = 12, ['d'] = 13, ['e'] = 14, ['f'] = 15,
    ['A'] = 10, ['B'] = 11, ['C'] = 12, ['D'] = 13, ['E'] = 14, ['F'] = 15,
    /* clang-format on */
};

S2N_RESULT s2n_hex_digit(uint8_t half_byte, uint8_t *hex_digit)
{
    RESULT_ENSURE_REF(hex_digit);
    RESULT_ENSURE(half_byte < s2n_array_len(value_to_hex), S2N_ERR_BAD_HEX);
    *hex_digit = value_to_hex[half_byte];
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_stuffer_hex_digit_from_char(uint8_t c, uint8_t *i)
{
    RESULT_ENSURE(c < s2n_array_len(hex_to_value), S2N_ERR_BAD_HEX);
    /* Invalid characters map to 0 in hex_to_value, but so does '0'. */
    if (hex_to_value[c] == 0) {
        RESULT_ENSURE(c == '0', S2N_ERR_BAD_HEX);
    }
    *i = hex_to_value[c];
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_stuffer_read_hex(struct s2n_stuffer *hex_in, const struct s2n_blob *bytes_out)
{
    RESULT_PRECONDITION(s2n_stuffer_validate(hex_in));
    RESULT_PRECONDITION(s2n_blob_validate(bytes_out));
    if (bytes_out->size == 0) {
        return S2N_RESULT_OK;
    }

    size_t hex_size = bytes_out->size * 2;
    RESULT_ENSURE(s2n_stuffer_data_available(hex_in) >= hex_size, S2N_ERR_BAD_HEX);

    uint8_t *out = bytes_out->data;
    uint8_t *in = hex_in->blob.data + hex_in->read_cursor;

    for (size_t i = 0; i < bytes_out->size; i++) {
        uint8_t hex_high = 0, hex_low = 0;
        RESULT_GUARD(s2n_stuffer_hex_digit_from_char(in[(i * 2)], &hex_high));
        RESULT_GUARD(s2n_stuffer_hex_digit_from_char(in[(i * 2) + 1], &hex_low));
        out[i] = (hex_high * 16) + hex_low;
    }

    RESULT_GUARD_POSIX(s2n_stuffer_skip_read(hex_in, hex_size));
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_stuffer_write_hex(struct s2n_stuffer *hex_out, const struct s2n_blob *bytes_in)
{
    RESULT_PRECONDITION(s2n_stuffer_validate(hex_out));
    RESULT_PRECONDITION(s2n_blob_validate(bytes_in));

    size_t bytes_size = bytes_in->size;
    size_t hex_size = bytes_size * 2;

    RESULT_GUARD_POSIX(s2n_stuffer_reserve_space(hex_out, hex_size));
    uint8_t *out = hex_out->blob.data + hex_out->write_cursor;
    uint8_t *in = bytes_in->data;

    for (size_t i = 0; i < bytes_size; i++) {
        out[(i * 2)] = value_to_hex[(in[i] >> 4)];
        out[(i * 2) + 1] = value_to_hex[(in[i] & 0x0f)];
    }

    RESULT_GUARD_POSIX(s2n_stuffer_skip_write(hex_out, hex_size));
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_stuffer_hex_read_n_bytes(struct s2n_stuffer *stuffer, uint8_t n, uint64_t *u)
{
    RESULT_ENSURE_LTE(n, sizeof(uint64_t));
    RESULT_ENSURE_REF(u);

    uint8_t hex_data[16] = { 0 };
    struct s2n_blob b = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&b, hex_data, n * 2));

    RESULT_ENSURE_REF(stuffer);
    RESULT_ENSURE(s2n_stuffer_read(stuffer, &b) == S2N_SUCCESS, S2N_ERR_BAD_HEX);

    /* Start with u = 0 */
    *u = 0;
    for (size_t i = 0; i < b.size; i++) {
        *u <<= 4;
        uint8_t hex = 0;
        RESULT_GUARD(s2n_stuffer_hex_digit_from_char(b.data[i], &hex));
        *u += hex;
    }

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_stuffer_read_uint16_hex(struct s2n_stuffer *stuffer, uint16_t *u)
{
    RESULT_ENSURE_REF(u);
    uint64_t u64 = 0;
    RESULT_GUARD(s2n_stuffer_hex_read_n_bytes(stuffer, sizeof(uint16_t), &u64));
    RESULT_ENSURE_LTE(u64, UINT16_MAX);
    *u = u64;
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_stuffer_read_uint8_hex(struct s2n_stuffer *stuffer, uint8_t *u)
{
    RESULT_ENSURE_REF(u);
    uint64_t u64 = 0;
    RESULT_GUARD(s2n_stuffer_hex_read_n_bytes(stuffer, sizeof(uint8_t), &u64));
    RESULT_ENSURE_LTE(u64, UINT8_MAX);
    *u = u64;
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_stuffer_hex_write_n_bytes(struct s2n_stuffer *stuffer, uint8_t n, uint64_t u)
{
    RESULT_ENSURE_LTE(n, sizeof(uint64_t));

    uint8_t hex_data[16] = { 0 };
    struct s2n_blob b = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&b, hex_data, n * 2));

    for (size_t i = b.size; i > 0; i--) {
        b.data[i - 1] = value_to_hex[u & 0x0f];
        u >>= 4;
    }

    RESULT_GUARD_POSIX(s2n_stuffer_write(stuffer, &b));
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_stuffer_write_uint16_hex(struct s2n_stuffer *stuffer, uint16_t u)
{
    return s2n_stuffer_hex_write_n_bytes(stuffer, sizeof(uint16_t), u);
}

S2N_RESULT s2n_stuffer_write_uint8_hex(struct s2n_stuffer *stuffer, uint8_t u)
{
    return s2n_stuffer_hex_write_n_bytes(stuffer, sizeof(uint8_t), u);
}
