/*
 * Copyright (c) 2017-2025 [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1.  Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 * 2.  Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef RNP_FINGERPRINT_HPP_
#define RNP_FINGERPRINT_HPP_

#include <array>
#include <vector>
#include <cstring>
#include "config.h"
#include <repgp/repgp_def.h>

typedef struct pgp_key_pkt_t pgp_key_pkt_t;

/* Size of the keyid */
#define PGP_KEY_ID_SIZE 8

/* Size of the fingerprint */
#define PGP_FINGERPRINT_V3_SIZE 16
#define PGP_FINGERPRINT_V4_SIZE 20
#define PGP_FINGERPRINT_V5_SIZE 32
#define PGP_MAX_FINGERPRINT_SIZE 32
#define PGP_FINGERPRINT_HEX_SIZE (PGP_MAX_FINGERPRINT_SIZE * 2) + 1

static_assert(PGP_MAX_FINGERPRINT_SIZE >= PGP_FINGERPRINT_V4_SIZE, "FP size mismatch.");
static_assert(PGP_MAX_FINGERPRINT_SIZE >= PGP_FINGERPRINT_V5_SIZE, "FP size mismatch.");
#if defined(ENABLE_CRYPTO_REFRESH)
#define PGP_FINGERPRINT_V6_SIZE 32
static_assert(PGP_MAX_FINGERPRINT_SIZE >= PGP_FINGERPRINT_V6_SIZE, "FP size mismatch.");
static_assert(PGP_FINGERPRINT_V5_SIZE == PGP_FINGERPRINT_V6_SIZE, "FP size mismatch.");
#endif

namespace pgp {

using KeyID = std::array<uint8_t, PGP_KEY_ID_SIZE>;
using KeyGrip = std::array<uint8_t, PGP_KEY_GRIP_SIZE>;

class Fingerprint {
    std::vector<uint8_t> fp_;
    KeyID                keyid_;

  public:
    Fingerprint();
    Fingerprint(const uint8_t *data, size_t size);
    Fingerprint(const pgp_key_pkt_t &src);

    bool operator==(const Fingerprint &src) const;
    bool operator!=(const Fingerprint &src) const;

    static bool                 size_valid(size_t size) noexcept;
    const KeyID &               keyid() const noexcept;
    const std::vector<uint8_t> &vec() const noexcept;
    const uint8_t *             data() const noexcept;
    size_t                      size() const noexcept;
};

using Fingerprints = std::vector<Fingerprint>;

} // namespace pgp

namespace std {
template <> struct hash<pgp::Fingerprint> {
    std::size_t
    operator()(pgp::Fingerprint const &fp) const noexcept
    {
        /* since fingerprint value is hash itself, we may use its low bytes */
        size_t res = 0;
        size_t cpy = std::min(sizeof(res), fp.size());
        std::memcpy(&res, fp.data(), cpy);
        return res;
    }
};
} // namespace std

#endif
