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

#include <string.h>
#include <cassert>
#include "crypto/hash.hpp"
#include <librepgp/stream-key.h>
#include <librepgp/stream-sig.h>
#include <librepgp/stream-packet.h>
#include "utils.h"
#include "fingerprint.hpp"

namespace pgp {

Fingerprint::Fingerprint() : keyid_({})
{
}

Fingerprint::Fingerprint(const uint8_t *data, size_t size) : Fingerprint()
{
    fp_.assign(data, data + size);
}

Fingerprint::Fingerprint(const pgp_key_pkt_t &key)
{
    switch (key.version) {
    case PGP_V2:
    case PGP_V3: {
        /* v2/3 fingerprint is calculated from RSA public numbers */
        if (!is_rsa_key_alg(key.alg)) {
            RNP_LOG("bad algorithm");
            throw rnp::rnp_exception(RNP_ERROR_NOT_SUPPORTED);
        }
        auto &rsa = dynamic_cast<const pgp::RSAKeyMaterial &>(*key.material);
        auto  hash = rnp::Hash::create(PGP_HASH_MD5);
        hash->add(rsa.n());
        hash->add(rsa.e());
        fp_ = hash->finish();
        /* keyid just low bytes of RSA n */
        size_t n = rsa.n().size();
        size_t sz = std::min(rsa.n().size(), keyid_.size());
        std::memcpy(keyid_.data(), rsa.n().data() + n - sz, sz);
        return;
    }
    case PGP_V4:
    case PGP_V5:
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_V6:
#endif
    {
        auto halg = key.version == PGP_V4 ? PGP_HASH_SHA1 : PGP_HASH_SHA256;
        auto hash = rnp::Hash::create(halg);
        signature_hash_key(key, *hash, key.version);
        fp_ = hash->finish();
        /* keyid */
        if (key.version == PGP_V4) {
            assert(fp_.size() == PGP_FINGERPRINT_V4_SIZE);
            const size_t inc = PGP_FINGERPRINT_V4_SIZE - PGP_KEY_ID_SIZE;
            memcpy(keyid_.data(), fp_.data() + inc, keyid_.size());
        } else {
            memcpy(keyid_.data(), fp_.data(), keyid_.size());
        }
        return;
    }
    default:
        RNP_LOG("unsupported key version");
        throw rnp::rnp_exception(RNP_ERROR_NOT_SUPPORTED);
    }
}

bool
Fingerprint::operator==(const Fingerprint &src) const
{
    return fp_ == src.fp_;
}

bool
Fingerprint::operator!=(const Fingerprint &src) const
{
    return !(*this == src);
}

bool
Fingerprint::size_valid(size_t size) noexcept
{
    return (size == PGP_FINGERPRINT_V4_SIZE) || (size == PGP_FINGERPRINT_V3_SIZE) ||
           (size == PGP_FINGERPRINT_V5_SIZE);
}

const KeyID &
Fingerprint::keyid() const noexcept
{
    return keyid_;
}

const std::vector<uint8_t> &
Fingerprint::vec() const noexcept
{
    return fp_;
}

const uint8_t *
Fingerprint::data() const noexcept
{
    return fp_.data();
}

size_t
Fingerprint::size() const noexcept
{
    return fp_.size();
}

} // namespace pgp
