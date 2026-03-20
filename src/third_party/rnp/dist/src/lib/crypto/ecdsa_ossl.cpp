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

#include "ecdsa.h"
#include "utils.h"
#include <string.h>
#include "ec_ossl.h"
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/ec.h>

namespace pgp {
namespace ecdsa {

static bool
decode_sig(const uint8_t *data, size_t len, ec::Signature &sig)
{
    rnp::ossl::ECDSASig esig(d2i_ECDSA_SIG(NULL, &data, len));
    if (!esig.get()) {
        RNP_LOG("Failed to parse ECDSA sig: %lu", ERR_peek_last_error());
        return false;
    }
    rnp::bn r, s;
    ECDSA_SIG_get0(esig.get(), r.cptr(), s.cptr());
    r.mpi(sig.r);
    s.mpi(sig.s);
    return true;
}

static bool
encode_sig(uint8_t *data, size_t *len, const ec::Signature &sig)
{
    ECDSA_SIG *dsig = ECDSA_SIG_new();
    rnp::bn    r(sig.r);
    rnp::bn    s(sig.s);
    if (!dsig || !r || !s) {
        RNP_LOG("Allocation failed.");
        ECDSA_SIG_free(dsig);
        return false;
    }
    ECDSA_SIG_set0(dsig, r.own(), s.own());
    int outlen = i2d_ECDSA_SIG(dsig, &data);
    ECDSA_SIG_free(dsig);
    if (outlen < 0) {
        RNP_LOG("Failed to encode signature.");
        return false;
    }
    *len = outlen;
    return true;
}

rnp_result_t
validate_key(rnp::RNG &rng, const ec::Key &key, bool secret)
{
    return ec::validate_key(key, secret);
}

rnp_result_t
sign(rnp::RNG &               rng,
     ec::Signature &          sig,
     pgp_hash_alg_t           hash_alg,
     const rnp::secure_bytes &hash,
     const ec::Key &          key)
{
    if (!key.x.size()) {
        RNP_LOG("private key not set");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* Load secret key to DSA structure*/
    auto evpkey = ec::load_key(key.p, &key.x, key.curve);
    if (!evpkey) {
        RNP_LOG("Failed to load key");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* init context and sign */
    rnp::ossl::evp::PKeyCtx ctx(EVP_PKEY_CTX_new(evpkey.get(), NULL));
    if (!ctx) {
        RNP_LOG("Context allocation failed: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
    }
    if (EVP_PKEY_sign_init(ctx.get()) <= 0) {
        RNP_LOG("Failed to initialize signing: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
    }
    /* DER encoded {r, s} pair, 16 bytes of overhead is enough */
    std::vector<uint8_t> dersig(2 * MAX_CURVE_BYTELEN + 16, 0);
    size_t               siglen = dersig.size();
    if (EVP_PKEY_sign(ctx.get(), dersig.data(), &siglen, hash.data(), hash.size()) <= 0) {
        RNP_LOG("Signing failed: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
    }
    if (!decode_sig(dersig.data(), siglen, sig)) {
        RNP_LOG("Failed to parse ECDSA sig: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
    }
    return RNP_SUCCESS;
}

rnp_result_t
verify(const ec::Signature &    sig,
       pgp_hash_alg_t           hash_alg,
       const rnp::secure_bytes &hash,
       const ec::Key &          key)
{
    /* Load secret key to DSA structure*/
    auto evpkey = ec::load_key(key.p, NULL, key.curve);
    if (!evpkey) {
        RNP_LOG("Failed to load key");
        return RNP_ERROR_BAD_PARAMETERS;
    }
    /* init context and sign */
    rnp::ossl::evp::PKeyCtx ctx(EVP_PKEY_CTX_new(evpkey.get(), NULL));
    if (!ctx) {
        RNP_LOG("Context allocation failed: %lu", ERR_peek_last_error());
        return RNP_ERROR_SIGNATURE_INVALID;
    }
    if (EVP_PKEY_verify_init(ctx.get()) <= 0) {
        RNP_LOG("Failed to initialize verify: %lu", ERR_peek_last_error());
        return RNP_ERROR_SIGNATURE_INVALID;
    }
    /* signature is two coordinates + DER encoding, 16 is safe enough */
    std::vector<uint8_t> sigbuf(sig.r.size() + sig.s.size() + 16);
    size_t               siglen = sigbuf.size();
    if (!encode_sig(sigbuf.data(), &siglen, sig)) {
        return RNP_ERROR_SIGNATURE_INVALID;
    }
    if (EVP_PKEY_verify(ctx.get(), sigbuf.data(), siglen, hash.data(), hash.size()) < 1) {
        return RNP_ERROR_SIGNATURE_INVALID;
    }
    return RNP_SUCCESS;
}
} // namespace ecdsa
} // namespace pgp
