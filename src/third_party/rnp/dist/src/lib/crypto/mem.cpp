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
#include "mem.h"
#include "logging.h"
#include <botan/ffi.h>

void
secure_clear(void *vp, size_t size)
{
    botan_scrub_mem(vp, size);
}

namespace rnp {

bool
hex_encode(const uint8_t *buf, size_t buf_len, char *hex, size_t hex_len, HexFormat format)
{
    uint32_t flags = format == HexFormat::Lowercase ? BOTAN_FFI_HEX_LOWER_CASE : 0;

    if (hex_len < (buf_len * 2 + 1)) {
        return false;
    }
    hex[buf_len * 2] = '\0';
    return botan_hex_encode(buf, buf_len, hex, flags) == 0;
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
    if (botan_hex_decode(hex, hexlen, buf, &buf_len) != 0) {
        RNP_LOG("Hex decode failed on string: %s", hex);
        return 0;
    }
    return buf_len;
}
} // namespace rnp