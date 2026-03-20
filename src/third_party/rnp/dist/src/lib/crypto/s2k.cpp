/*
 * Copyright (c) 2017-2022 [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * This code is originally derived from software contributed to
 * The NetBSD Foundation by Alistair Crooks (agc@netbsd.org), and
 * carried further by Ribose Inc (https://www.ribose.com).
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

#include <stdio.h>
#include "config.h"
#ifndef _MSC_VER
#include <sys/time.h>
#else
#include "uniwin.h"
#endif

#include "crypto/s2k.h"
#include "defaults.h"
#include "rnp.h"
#include "types.h"
#include "utils.h"
#ifdef CRYPTO_BACKEND_BOTAN
#include <botan/ffi.h>
#include "hash_botan.hpp"
#endif

bool
pgp_s2k_derive_key(pgp_s2k_t *s2k, const char *password, uint8_t *key, int keysize)
{
    uint8_t *saltptr = NULL;
    unsigned iterations = 1;

    switch (s2k->specifier) {
    case PGP_S2KS_SIMPLE:
        break;
    case PGP_S2KS_SALTED:
        saltptr = s2k->salt;
        break;
    case PGP_S2KS_ITERATED_AND_SALTED:
        saltptr = s2k->salt;
        if (s2k->iterations < 256) {
            iterations = pgp_s2k_decode_iterations(s2k->iterations);
        } else {
            iterations = s2k->iterations;
        }
        break;
    default:
        return false;
    }

    if (pgp_s2k_iterated(s2k->hash_alg, key, keysize, password, saltptr, iterations)) {
        RNP_LOG("s2k failed");
        return false;
    }

    return true;
}

#ifdef CRYPTO_BACKEND_BOTAN
int
pgp_s2k_iterated(pgp_hash_alg_t alg,
                 uint8_t *      out,
                 size_t         output_len,
                 const char *   password,
                 const uint8_t *salt,
                 size_t         iterations)
{
    char s2k_algo_str[128];
    snprintf(s2k_algo_str,
             sizeof(s2k_algo_str),
             "OpenPGP-S2K(%s)",
             rnp::Hash_Botan::name_backend(alg));

    return botan_pwdhash(s2k_algo_str,
                         iterations,
                         0,
                         0,
                         out,
                         output_len,
                         password,
                         0,
                         salt,
                         salt ? PGP_SALT_SIZE : 0);
}
#endif

size_t
pgp_s2k_decode_iterations(uint8_t c)
{
    // See RFC 4880 section 3.7.1.3
    return (16 + (c & 0x0F)) << ((c >> 4) + 6);
}

size_t
pgp_s2k_round_iterations(size_t iterations)
{
    return pgp_s2k_decode_iterations(pgp_s2k_encode_iterations(iterations));
}

uint8_t
pgp_s2k_encode_iterations(size_t iterations)
{
    /* For compatibility, when an S2K specifier is used, the special value
     * 254 or 255 is stored in the position where the hash algorithm octet
     * would have been in the old data structure. This is then followed
     * immediately by a one-octet algorithm identifier, and then by the S2K
     * specifier as encoded above.
     * 0:           secret data is unencrypted (no password)
     * 255 or 254:  followed by algorithm octet and S2K specifier
     * Cipher alg:  use Simple S2K algorithm using MD5 hash
     * For more info refer to rfc 4880 section 3.7.2.1.
     */
    for (uint16_t c = 0; c < 256; ++c) {
        // This could be a binary search
        if (pgp_s2k_decode_iterations(c) >= iterations) {
            return c;
        }
    }
    return 255;
}

/// Should this function be elsewhere?
static uint64_t
get_timestamp_usec()
{
#ifndef _MSC_VER
    // TODO: Consider clock_gettime
    struct timeval tv;
    ::gettimeofday(&tv, NULL);
    return (static_cast<uint64_t>(tv.tv_sec) * 1000000) + static_cast<uint64_t>(tv.tv_usec);
#else
    return GetTickCount64() * 1000;
#endif
}

size_t
pgp_s2k_compute_iters(pgp_hash_alg_t alg, size_t desired_msec, size_t trial_msec)
{
    if (desired_msec == 0) {
        desired_msec = DEFAULT_S2K_MSEC;
    }
    if (trial_msec == 0) {
        trial_msec = DEFAULT_S2K_TUNE_MSEC;
    }

    // number of iterations to estimate the number of iterations
    // (sorry, cannot tell it better)
    const uint8_t NUM_ITERATIONS = 16;
    uint64_t      duration = 0;
    size_t        bytes = 0;
    try {
        for (uint8_t i = 0; i < NUM_ITERATIONS; i++) {
            uint64_t start = get_timestamp_usec();
            uint64_t end = start;
            auto     hash = rnp::Hash::create(alg);
            uint8_t  buf[8192] = {0};
            while (end - start < trial_msec * 1000ull) {
                hash->add(buf, sizeof(buf));
                bytes += sizeof(buf);
                end = get_timestamp_usec();
            }
            hash->finish(buf);
            duration += (end - start);
        }
    } catch (const std::exception &e) {
        RNP_LOG("Failed to hash data: %s", e.what());
        return 0;
    }

    const uint8_t MIN_ITERS = 96;
    if (duration == 0) {
        return pgp_s2k_decode_iterations(MIN_ITERS);
    }

    const double  bytes_per_usec = static_cast<double>(bytes) / duration;
    const double  desired_usec = desired_msec * 1000.0;
    const double  bytes_for_target = bytes_per_usec * desired_usec;
    const uint8_t iters = pgp_s2k_encode_iterations(bytes_for_target);

    return pgp_s2k_decode_iterations((iters > MIN_ITERS) ? iters : MIN_ITERS);
}
