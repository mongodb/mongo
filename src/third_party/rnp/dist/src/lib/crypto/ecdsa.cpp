/*
 * Copyright (c) 2017-2024, [Ribose Inc](https://www.ribose.com).
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

#include "ecdsa.h"
#include "utils.h"
#include <botan/ffi.h>
#include <string.h>
#include "botan_utils.hpp"

namespace pgp {
namespace ecdsa {

static bool
load_public_key(rnp::botan::Pubkey &pubkey, const ec::Key &keydata)
{
    auto curve = ec::Curve::get(keydata.curve);
    if (!curve) {
        RNP_LOG("unknown curve");
        return false;
    }
    if (!keydata.p.size() || (keydata.p[0] != 0x04)) {
        RNP_LOG("Failed to load public key: %02x", keydata.p[0]);
        return false;
    }
    const size_t curve_order = curve->bytes();
    if (keydata.p.size() != 2 * curve_order + 1) {
        return false;
    }
    rnp::bn px(&keydata.p[1], curve_order);
    rnp::bn py(&keydata.p[1] + curve_order, curve_order);

    if (!px || !py) {
        return false;
    }

    bool res = !botan_pubkey_load_ecdsa(&pubkey.get(), px.get(), py.get(), curve->botan_name);
    if (!res) {
        RNP_LOG("failed to load ecdsa %s public key", curve->botan_name);
    }
    return res;
}

static bool
load_secret_key(rnp::botan::Privkey &seckey, const ec::Key &keydata)
{
    auto curve = ec::Curve::get(keydata.curve);
    if (!curve) {
        return false;
    }

    rnp::bn x(keydata.x);
    if (!x) {
        return false;
    }

    bool res = !botan_privkey_load_ecdsa(&seckey.get(), x.get(), curve->botan_name);
    if (!res) {
        RNP_LOG("Can't load private %s key", curve->botan_name);
    }
    return res;
}

rnp_result_t
validate_key(rnp::RNG &rng, const ec::Key &key, bool secret)
{
    rnp::botan::Pubkey bpkey;
    if (!load_public_key(bpkey, key) || botan_pubkey_check_key(bpkey.get(), rng.handle(), 0)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!secret) {
        return RNP_SUCCESS;
    }

    rnp::botan::Privkey bskey;
    if (!load_secret_key(bskey, key) ||
        botan_privkey_check_key(bskey.get(), rng.handle(), 0)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return RNP_SUCCESS;
}

const char *
padding_str_for(pgp_hash_alg_t hash_alg)
{
    switch (hash_alg) {
    case PGP_HASH_MD5:
        return "Raw(MD5)";
    case PGP_HASH_SHA1:
        return "Raw(SHA-1)";
    case PGP_HASH_RIPEMD:
        return "Raw(RIPEMD-160)";
    case PGP_HASH_SHA256:
        return "Raw(SHA-256)";
    case PGP_HASH_SHA384:
        return "Raw(SHA-384)";
    case PGP_HASH_SHA512:
        return "Raw(SHA-512)";
    case PGP_HASH_SHA224:
        return "Raw(SHA-224)";
    case PGP_HASH_SHA3_256:
        return "Raw(SHA-3(256))";
    case PGP_HASH_SHA3_512:
        return "Raw(SHA-3(512))";
    case PGP_HASH_SM3:
        return "Raw(SM3)";
    default:
        return "Raw";
    }
}

rnp_result_t
sign(rnp::RNG &               rng,
     ec::Signature &          sig,
     pgp_hash_alg_t           hash_alg,
     const rnp::secure_bytes &hash,
     const ec::Key &          key)
{
    auto curve = ec::Curve::get(key.curve);
    if (!curve) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    rnp::botan::Privkey b_key;
    if (!load_secret_key(b_key, key)) {
        RNP_LOG("Can't load private key");
        return RNP_ERROR_GENERIC;
    }

    rnp::botan::op::Sign signer;
    auto                 pad = padding_str_for(hash_alg);
    if (botan_pk_op_sign_create(&signer.get(), b_key.get(), pad, 0) ||
        botan_pk_op_sign_update(signer.get(), hash.data(), hash.size())) {
        return RNP_ERROR_GENERIC;
    }

    const size_t         curve_order = curve->bytes();
    size_t               sig_len = 2 * curve_order;
    std::vector<uint8_t> out_buf(sig_len);

    if (botan_pk_op_sign_finish(signer.get(), rng.handle(), out_buf.data(), &sig_len)) {
        RNP_LOG("Signing failed");
        return RNP_ERROR_GENERIC;
    }

    // Allocate memory and copy results
    sig.r.assign(out_buf.data(), curve_order);
    sig.s.assign(out_buf.data() + curve_order, curve_order);
    return RNP_SUCCESS;
}

rnp_result_t
verify(const ec::Signature &    sig,
       pgp_hash_alg_t           hash_alg,
       const rnp::secure_bytes &hash,
       const ec::Key &          key)
{
    auto curve = ec::Curve::get(key.curve);
    if (!curve) {
        RNP_LOG("unknown curve");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    size_t curve_order = curve->bytes();
    size_t r_blen = sig.r.size();
    size_t s_blen = sig.s.size();
    if ((r_blen > curve_order) || (s_blen > curve_order) ||
        (curve_order > MAX_CURVE_BYTELEN)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    rnp::botan::Pubkey pub;
    if (!load_public_key(pub, key)) {
        return RNP_ERROR_SIGNATURE_INVALID;
    }

    rnp::botan::op::Verify verifier;
    auto                   pad = padding_str_for(hash_alg);
    if (botan_pk_op_verify_create(&verifier.get(), pub.get(), pad, 0) ||
        botan_pk_op_verify_update(verifier.get(), hash.data(), hash.size())) {
        return RNP_ERROR_SIGNATURE_INVALID;
    }

    std::vector<uint8_t> sign_buf(2 * curve_order, 0);
    // Both can't fail
    sig.r.copy(sign_buf.data() + curve_order - r_blen);
    sig.s.copy(sign_buf.data() + 2 * curve_order - s_blen);

    if (botan_pk_op_verify_finish(verifier.get(), sign_buf.data(), sign_buf.size())) {
        return RNP_ERROR_SIGNATURE_INVALID;
    }
    return RNP_SUCCESS;
}
} // namespace ecdsa
} // namespace pgp
