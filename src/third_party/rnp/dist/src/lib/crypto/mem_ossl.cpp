/*-
 * Copyright (c) 2021 Ribose Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <cstdio>
#include <cstring>
#include "mem.h"
#include "logging.h"
#include <openssl/crypto.h>

void
secure_clear(void *vp, size_t size)
{
    OPENSSL_cleanse(vp, size);
}

namespace rnp {

bool
hex_encode(const uint8_t *buf, size_t buf_len, char *hex, size_t hex_len, HexFormat format)
{
    if (hex_len < (buf_len * 2 + 1)) {
        return false;
    }
    static const char *hex_low = "0123456789abcdef";
    static const char *hex_up = "0123456789ABCDEF";
    const char *       hex_ch = (format == HexFormat::Lowercase) ? hex_low : hex_up;
    hex[buf_len * 2] = '\0';
    for (size_t i = 0; i < buf_len; i++) {
        hex[i << 1] = hex_ch[buf[i] >> 4];
        hex[(i << 1) + 1] = hex_ch[buf[i] & 0xF];
    }
    return true;
}

static bool
hex_char_decode(const char hex, uint8_t &res)
{
    if ((hex >= '0') && (hex <= '9')) {
        res = hex - '0';
        return true;
    }
    if (hex >= 'a' && hex <= 'f') {
        res = hex + 10 - 'a';
        return true;
    }
    if (hex >= 'A' && hex <= 'F') {
        res = hex + 10 - 'A';
        return true;
    }
    return false;
}

size_t
hex_decode(const char *hex, uint8_t *buf, size_t buf_len)
{
    size_t hexlen = strlen(hex);

    /* check for 0x prefix */
    if ((hexlen >= 2) && (hex[0] == '0') && ((hex[1] == 'x') || (hex[1] == 'X'))) {
        hex += 2;
        hexlen -= 2;
    }
    const char *end = hex + hexlen;
    uint8_t *   buf_st = buf;
    uint8_t *   buf_en = buf + buf_len;
    while (hex < end) {
        /* skip whitespaces */
        if ((*hex < '0') &&
            ((*hex == ' ') || (*hex == '\t') || (*hex == '\r') || (*hex == '\n'))) {
            hex++;
            continue;
        }
        /* We assume that spaces/tabs divide hex string between even groups of hex chars */
        if (hex + 2 > end) {
            RNP_LOG("Invalid hex string length.");
            return 0;
        }
        uint8_t lo, hi;
        if (!hex_char_decode(*hex++, hi) || !hex_char_decode(*hex++, lo)) {
            RNP_LOG("Hex decode failed on string: %s", hex);
            return 0;
        }
        if (buf == buf_en) {
            return 0;
        }
        *buf++ = (hi << 4) | lo;
    }
    return buf - buf_st;
}

} // namespace rnp
