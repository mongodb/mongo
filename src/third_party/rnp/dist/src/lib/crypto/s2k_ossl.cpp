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

#include <cstdint>
#include <vector>
#include <algorithm>
#include <openssl/evp.h>
#include "hash.hpp"
#include "s2k.h"
#include "mem.h"
#include "logging.h"

int
pgp_s2k_iterated(pgp_hash_alg_t alg,
                 uint8_t *      out,
                 size_t         output_len,
                 const char *   password,
                 const uint8_t *salt,
                 size_t         iterations)
{
    if ((iterations > 1) && !salt) {
        RNP_LOG("Iterated S2K mus be salted as well.");
        return 1;
    }
    size_t hash_len = rnp::Hash::size(alg);
    if (!hash_len) {
        RNP_LOG("Unknown digest: %d", (int) alg);
        return 1;
    }
    try {
        size_t pswd_len = strlen(password);
        size_t salt_len = salt ? PGP_SALT_SIZE : 0;

        rnp::secure_bytes data(salt_len + pswd_len);
        if (salt_len) {
            memcpy(data.data(), salt, PGP_SALT_SIZE);
        }
        memcpy(data.data() + salt_len, password, pswd_len);
        size_t zeroes = 0;

        while (output_len) {
            /* create hash context */
            auto hash = rnp::Hash::create(alg);
            /* add leading zeroes */
            hash->add(std::vector<uint8_t>(zeroes, 0));
            if (!data.empty()) {
                /* if iteration is 1 then still hash the whole data chunk */
                size_t left = std::max(data.size(), iterations);
                while (left) {
                    size_t to_hash = std::min(left, data.size());
                    hash->add(data.data(), to_hash);
                    left -= to_hash;
                }
            }
            auto   dgst = hash->sec_finish();
            size_t out_cpy = std::min(dgst.size(), output_len);
            memcpy(out, dgst.data(), out_cpy);
            output_len -= out_cpy;
            out += out_cpy;
            zeroes++;
        }
        return 0;
    } catch (const std::exception &e) {
        RNP_LOG("s2k failed: %s", e.what());
        return 1;
    }
}
