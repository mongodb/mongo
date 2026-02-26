/*
 * Copyright (c) 2021-2024, [Ribose Inc](https://www.ribose.com).
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

#include <string>
#include <cassert>
#include "eddsa.h"
#include "ec.h"
#include "ec_ossl.h"
#include "utils.h"
#include "ossl_utils.hpp"
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/err.h>
#include <openssl/ec.h>

namespace pgp {
namespace eddsa {

rnp_result_t
validate_key(rnp::RNG &rng, const ec::Key &key, bool secret)
{
    /* Not implemented in the OpenSSL, so just do basic size checks. */
    if ((key.p.size() != 33) || (key.p[0] != 0x40)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (secret && key.x.size() > 32) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return RNP_SUCCESS;
}

rnp_result_t
generate(rnp::RNG &rng, ec::Key &key)
{
    rnp_result_t ret = key.generate(rng, PGP_PKA_EDDSA, PGP_CURVE_ED25519);
    if (!ret) {
        key.curve = PGP_CURVE_ED25519;
    }
    return ret;
}

rnp_result_t
verify(const ec::Signature &sig, const rnp::secure_bytes &hash, const ec::Key &key)
{
    if ((sig.r.size() > 32) || (sig.s.size() > 32)) {
        RNP_LOG("Invalid EdDSA signature.");
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if ((key.p.size() != 33) || (key.p[0] != 0x40)) {
        RNP_LOG("Invalid EdDSA public key.");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    auto evpkey = ec::load_key(key.p, NULL, PGP_CURVE_ED25519);
    if (!evpkey) {
        RNP_LOG("Failed to load key");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* init context and sign */
    rnp::ossl::evp::MDCtx md(EVP_MD_CTX_new());
    if (!md) {
        RNP_LOG("Failed to allocate MD ctx: %lu", ERR_peek_last_error());
        return RNP_ERROR_SIGNATURE_INVALID;
    }
    /* ctx will be destroyed together with md */
    EVP_PKEY_CTX *ctx = NULL;
    if (EVP_DigestVerifyInit(md.get(), &ctx, NULL, NULL, evpkey.get()) <= 0) {
        RNP_LOG("Failed to initialize signing: %lu", ERR_peek_last_error());
        return RNP_ERROR_SIGNATURE_INVALID;
    }
    std::array<uint8_t, 64> sigbuf{};
    sig.r.copy(&sigbuf[32 - sig.r.size()]);
    sig.s.copy(&sigbuf[64 - sig.s.size()]);

    if (EVP_DigestVerify(md.get(), sigbuf.data(), 64, hash.data(), hash.size()) < 1) {
        return RNP_ERROR_SIGNATURE_INVALID;
    }
    return RNP_SUCCESS;
}

rnp_result_t
sign(rnp::RNG &rng, ec::Signature &sig, const rnp::secure_bytes &hash, const ec::Key &key)
{
    if (!key.x.size()) {
        RNP_LOG("private key not set");
        return RNP_ERROR_BAD_PARAMETERS;
    }
    auto evpkey = ec::load_key(key.p, &key.x, PGP_CURVE_ED25519);
    if (!evpkey) {
        RNP_LOG("Failed to load private key: %lu", ERR_peek_last_error());
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* init context and sign */
    rnp::ossl::evp::MDCtx md(EVP_MD_CTX_new());
    if (!md) {
        RNP_LOG("Failed to allocate MD ctx: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
    }
    /* ctx will be destroyed together with md */
    EVP_PKEY_CTX *ctx = NULL;
    if (EVP_DigestSignInit(md.get(), &ctx, NULL, NULL, evpkey.get()) <= 0) {
        RNP_LOG("Failed to initialize signing: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
    }
    std::array<uint8_t, 64> sigbuf{};
    size_t                  siglen = sigbuf.size();
    if (EVP_DigestSign(md.get(), sigbuf.data(), &siglen, hash.data(), hash.size()) <= 0) {
        RNP_LOG("Signing failed: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
    }
    assert(siglen == 64);
    sig.r.assign(sigbuf.data(), 32);
    sig.s.assign(sigbuf.data() + 32, 32);
    return RNP_SUCCESS;
}
} // namespace eddsa
} // namespace pgp
