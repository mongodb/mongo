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

#include <string.h>

#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "utils/s2n_safety.h"

static const uint8_t b64[64] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q',
    'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
    'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+',
    '/' };

/* Generated with this python:
 *
 * b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
 *
 * for i in range(0, 256):
 *     if chr(i) in b64:
 *         print str(b64.index(chr(i))) + ", ",
 *      else:
 *         print "255, ",
 *
 *      if (i + 1) % 16 == 0:
 *          print
 *
 * Note that '=' maps to 64.
 */
static const uint8_t b64_inverse[256] = { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 62, 255, 255, 255, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 255, 255, 255, 64, 255, 255,
    255, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 255, 255, 255,
    255, 255, 255, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50,
    51, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 };

bool s2n_is_base64_char(unsigned char c)
{
    return (b64_inverse[*((uint8_t *) (&c))] != 255);
}

/**
 * NOTE:
 * In general, shift before masking. This avoids needing to worry about how the
 * signed bit may be handled.
 */
int s2n_stuffer_read_base64(struct s2n_stuffer *stuffer, struct s2n_stuffer *out)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_PRECONDITION(s2n_stuffer_validate(out));
    int bytes_this_round = 3;
    s2n_stack_blob(o, 4, 4);

    do {
        if (s2n_stuffer_data_available(stuffer) < o.size) {
            break;
        }

        POSIX_GUARD(s2n_stuffer_read(stuffer, &o));

        uint8_t value1 = b64_inverse[o.data[0]];
        uint8_t value2 = b64_inverse[o.data[1]];
        uint8_t value3 = b64_inverse[o.data[2]];
        uint8_t value4 = b64_inverse[o.data[3]];

        /* We assume the entire thing is base64 data, thus, terminate cleanly if we encounter a non-base64 character */
        if (value1 == 255) {
            /* Undo the read */
            stuffer->read_cursor -= o.size;
            POSIX_BAIL(S2N_ERR_INVALID_BASE64);
        }

        /* The first two characters can never be '=' and in general
         * everything has to be a valid character.
         */
        POSIX_ENSURE(!(value1 == 64 || value2 == 64 || value2 == 255 || value3 == 255 || value4 == 255),
                S2N_ERR_INVALID_BASE64);

        if (o.data[2] == '=') {
            /* If there is only one output byte, then the second value
             * should have none of its bottom four bits set.
             */
            POSIX_ENSURE(!(o.data[3] != '=' || value2 & 0x0f), S2N_ERR_INVALID_BASE64);
            bytes_this_round = 1;
            value3 = 0;
            value4 = 0;
        } else if (o.data[3] == '=') {
            /* The last two bits of the final value should be unset */
            POSIX_ENSURE(!(value3 & 0x03), S2N_ERR_INVALID_BASE64);

            bytes_this_round = 2;
            value4 = 0;
        }

        /* Advance by bytes_this_round, and then fill in the data */
        POSIX_GUARD(s2n_stuffer_skip_write(out, bytes_this_round));
        uint8_t *ptr = out->blob.data + out->write_cursor - bytes_this_round;

        /* value1 maps to the first 6 bits of the first data byte */
        /* value2's top two bits are the rest */
        *ptr = ((value1 << 2) & 0xfc) | ((value2 >> 4) & 0x03);

        if (bytes_this_round > 1) {
            /* Put the next four bits in the second data byte */
            /* Put the next four bits in the third data byte */
            ptr++;
            *ptr = ((value2 << 4) & 0xf0) | ((value3 >> 2) & 0x0f);
        }

        if (bytes_this_round > 2) {
            /* Put the next two bits in the third data byte */
            /* Put the next six bits in the fourth data byte */
            ptr++;
            *ptr = ((value3 << 6) & 0xc0) | (value4 & 0x3f);
        }
    } while (bytes_this_round == 3);

    return S2N_SUCCESS;
}

int s2n_stuffer_write_base64(struct s2n_stuffer *stuffer, struct s2n_stuffer *in)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_PRECONDITION(s2n_stuffer_validate(in));
    s2n_stack_blob(o, 4, 4);
    s2n_stack_blob(i, 3, 3);

    while (s2n_stuffer_data_available(in) > 2) {
        POSIX_GUARD(s2n_stuffer_read(in, &i));

        /* Take the top 6-bits of the first data byte  */
        o.data[0] = b64[(i.data[0] >> 2) & 0x3f];

        /* Take the bottom 2-bits of the first data byte -  0b00110000 = 0x30
         * and take the top 4-bits of the second data byte - 0b00001111 = 0x0f
         */
        o.data[1] = b64[((i.data[0] << 4) & 0x30) | ((i.data[1] >> 4) & 0x0f)];

        /* Take the bottom 4-bits of the second data byte - 0b00111100 = 0x3c
         * and take the top 2-bits of the third data byte - 0b00000011 = 0x03
         */
        o.data[2] = b64[((i.data[1] << 2) & 0x3c) | ((i.data[2] >> 6) & 0x03)];

        /* Take the bottom 6-bits of the second data byte - 0b00111111 = 0x3f
         */
        o.data[3] = b64[i.data[2] & 0x3f];

        POSIX_GUARD(s2n_stuffer_write(stuffer, &o));
    }

    if (s2n_stuffer_data_available(in)) {
        /* Read just one byte */
        i.size = 1;
        POSIX_GUARD(s2n_stuffer_read(in, &i));
        uint8_t c = i.data[0];

        /* We at least one data byte left to encode, encode
         * its first six bits
         */
        o.data[0] = b64[(c >> 2) & 0x3f];

        /* And our end has to be an equals */
        o.data[3] = '=';

        /* How many bytes are actually left? */
        if (s2n_stuffer_data_available(in) == 0) {
            /* We just have the last two bits to deal with */
            o.data[1] = b64[(c << 4) & 0x30];
            o.data[2] = '=';
        } else {
            /* Read the last byte */
            POSIX_GUARD(s2n_stuffer_read(in, &i));

            o.data[1] = b64[((c << 4) & 0x30) | ((i.data[0] >> 4) & 0x0f)];
            o.data[2] = b64[((i.data[0] << 2) & 0x3c)];
        }

        POSIX_GUARD(s2n_stuffer_write(stuffer, &o));
    }

    return S2N_SUCCESS;
}
