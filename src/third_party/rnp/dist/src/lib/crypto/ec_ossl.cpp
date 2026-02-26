/*
 * Copyright (c) 2021-2024 [Ribose Inc](https://www.ribose.com).
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
#include <algorithm>
#include "ec.h"
#include "ec_ossl.h"
#include "types.h"
#include "mem.h"
#include "utils.h"
#include "ossl_utils.hpp"
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/err.h>
#include <openssl/ec.h>
#if defined(CRYPTO_BACKEND_OPENSSL3)
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#endif

namespace pgp {
namespace ec {

static bool
is_raw_key(const pgp_curve_t curve)
{
    return (curve == PGP_CURVE_ED25519) || (curve == PGP_CURVE_25519);
}

rnp_result_t
Key::generate_x25519(rnp::RNG &rng)
{
    return generate(rng, PGP_PKA_ECDH, PGP_CURVE_25519);
}

rnp::ossl::evp::PKey
generate_pkey(const pgp_pubkey_alg_t alg_id, const pgp_curve_t curve)
{
    if (!Curve::alg_allows(alg_id, curve)) {
        return nullptr;
    }
    auto ec_desc = Curve::get(curve);
    if (!ec_desc) {
        return nullptr;
    }
    int nid = OBJ_sn2nid(ec_desc->openssl_name);
    if (nid == NID_undef) {
        /* LCOV_EXCL_START */
        RNP_LOG("Unknown SN: %s", ec_desc->openssl_name);
        return nullptr;
        /* LCOV_EXCL_END */
    }
    bool                    raw = is_raw_key(curve);
    rnp::ossl::evp::PKeyCtx ctx(EVP_PKEY_CTX_new_id(raw ? nid : EVP_PKEY_EC, NULL));
    if (!ctx) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to create ctx: %lu", ERR_peek_last_error());
        return nullptr;
        /* LCOV_EXCL_END */
    }
    if (EVP_PKEY_keygen_init(ctx.get()) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to init keygen: %lu", ERR_peek_last_error());
        return nullptr;
        /* LCOV_EXCL_END */
    }
    if (!raw && (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(), nid) <= 0)) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to set curve nid: %lu", ERR_peek_last_error());
        return nullptr;
        /* LCOV_EXCL_END */
    }
    EVP_PKEY *rawkey = NULL;
    if (EVP_PKEY_keygen(ctx.get(), &rawkey) <= 0) {
        RNP_LOG("EC keygen failed: %lu", ERR_peek_last_error()); // LCOV_EXCL_LINE
    }
    return rnp::ossl::evp::PKey(rawkey);
}

static bool
write_raw_seckey(const rnp::ossl::evp::PKey &pkey, ec::Key &key)
{
    /* EdDSA and X25519 keys are saved in a different way */
    std::vector<uint8_t> raw(32, 0);
    size_t               rlen = raw.size();
    if (EVP_PKEY_get_raw_private_key(pkey.get(), raw.data(), &rlen) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed get raw private key: %lu", ERR_peek_last_error());
        return false;
        /* LCOV_EXCL_END */
    }
    assert(rlen == 32);
    raw.resize(rlen);
    if (EVP_PKEY_id(pkey.get()) == EVP_PKEY_X25519) {
        /* in OpenSSL private key is exported as little-endian, while MPI is big-endian */
        std::reverse(raw.begin(), raw.end());
    }
    key.x.assign(raw.data(), raw.size());
    return true;
}

static bool
write_seckey(rnp::ossl::evp::PKey &pkey, mpi &key)
{
#if defined(CRYPTO_BACKEND_OPENSSL3)
    rnp::bn x;
    return EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_PRIV_KEY, x.ptr()) && x.mpi(key);
#else
    auto ec = EVP_PKEY_get0_EC_KEY(pkey.get());
    if (!ec) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to retrieve EC key: %lu", ERR_peek_last_error());
        return false;
        /* LCOV_EXCL_END */
    }
    const rnp::bn x(EC_KEY_get0_private_key(ec));
    if (!x) {
        return false;
    }
    return x.mpi(key);
#endif
}

rnp_result_t
Key::generate(rnp::RNG &rng, const pgp_pubkey_alg_t alg_id, const pgp_curve_t curve)
{
    auto pkey = generate_pkey(alg_id, curve);
    if (!pkey) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (is_raw_key(curve)) {
        if (write_pubkey(pkey, p, curve) && write_raw_seckey(pkey, *this)) {
            return RNP_SUCCESS;
        }
        return RNP_ERROR_GENERIC;
    }
    if (!write_pubkey(pkey, p, curve)) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to write pubkey.");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    if (!write_seckey(pkey, x)) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to write seckey.");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    return RNP_SUCCESS;
}

static rnp::ossl::evp::PKey
load_raw_key(const mpi &keyp, const mpi *keyx, int nid)
{
    if (!keyx) {
        /* as per RFC, EdDSA & 25519 keys must use 0x40 byte for encoding */
        if ((keyp.size() != 33) || (keyp[0] != 0x40)) {
            RNP_LOG("Invalid 25519 public key. Size %zu, byte 0x%02x", keyp.size(), keyp[0]);
            return nullptr;
        }
        rnp::ossl::evp::PKey evpkey(
          EVP_PKEY_new_raw_public_key(nid, NULL, &keyp[1], keyp.size() - 1));
        if (!evpkey) {
            RNP_LOG("Failed to load public key: %lu", ERR_peek_last_error()); // LCOV_EXCL_LINE
        }
        return evpkey;
    }

    EVP_PKEY *evpkey = NULL;
    if (nid == EVP_PKEY_X25519) {
        if (keyx->size() != 32) {
            RNP_LOG("Invalid 25519 secret key");
            return nullptr;
        }
        /* need to reverse byte order since in mpi we have big-endian */
        rnp::secure_bytes prkey(keyx->data(), keyx->data() + keyx->size());
        std::reverse(prkey.begin(), prkey.end());
        evpkey = EVP_PKEY_new_raw_private_key(nid, NULL, prkey.data(), prkey.size());
    } else {
        if (keyx->size() > 32) {
            RNP_LOG("Invalid Ed25519 secret key");
            return nullptr;
        }
        /* keyx->size() may be smaller then 32 as high byte is random and could become 0 */
        rnp::secure_array<uint8_t, 32> prkey{};
        memcpy(prkey.data() + 32 - keyx->size(), keyx->data(), keyx->size());
        evpkey = EVP_PKEY_new_raw_private_key(nid, NULL, prkey.data(), 32);
    }
    if (!evpkey) {
        RNP_LOG("Failed to load private key: %lu", ERR_peek_last_error()); // LCOV_EXCL_LINE
    }
    return rnp::ossl::evp::PKey(evpkey);
}

#if defined(CRYPTO_BACKEND_OPENSSL3)
static rnp::ossl::Param
build_params(const mpi &p, const mpi *x, const char *curve)
{
    rnp::ossl::ParamBld bld(OSSL_PARAM_BLD_new());
    if (!bld) {
        return nullptr;
    }
    rnp::bn bx(x);
    if (!OSSL_PARAM_BLD_push_utf8_string(bld.get(), OSSL_PKEY_PARAM_GROUP_NAME, curve, 0) ||
        !OSSL_PARAM_BLD_push_octet_string(
          bld.get(), OSSL_PKEY_PARAM_PUB_KEY, p.data(), p.size()) ||
        (x && !OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_PRIV_KEY, bx.get()))) {
        return nullptr; // LCOV_EXCL_LINE
    }
    return rnp::ossl::Param(OSSL_PARAM_BLD_to_param(bld.get()));
}

static rnp::ossl::evp::PKey
load_key_openssl3(const mpi &keyp, const mpi *keyx, const Curve &curv_desc)
{
    auto params = build_params(keyp, keyx, curv_desc.openssl_name);
    if (!params) {
        /* LCOV_EXCL_START */
        RNP_LOG("failed to build ec params");
        return nullptr;
        /* LCOV_EXCL_END */
    }
    rnp::ossl::evp::PKeyCtx ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL));
    if (!ctx) {
        /* LCOV_EXCL_START */
        RNP_LOG("failed to create ec context");
        return nullptr;
        /* LCOV_EXCL_END */
    }
    EVP_PKEY *evpkey = NULL;
    int       sel = keyx ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY;
    if ((EVP_PKEY_fromdata_init(ctx.get()) != 1) ||
        (EVP_PKEY_fromdata(ctx.get(), &evpkey, sel, params.get()) != 1)) {
        /* LCOV_EXCL_START */
        RNP_LOG("failed to create ec key from data");
        /* Some version of OpenSSL may leave evpkey non-NULL after failure, so let's be safe */
        evpkey = NULL;
        /* LCOV_EXCL_END */
    }
    return rnp::ossl::evp::PKey(evpkey);
}
#endif

rnp::ossl::evp::PKey
load_key(const mpi &keyp, const mpi *keyx, pgp_curve_t curve)
{
    auto curv_desc = Curve::get(curve);
    if (!curv_desc) {
        RNP_LOG("unknown curve");
        return nullptr;
    }
    if (!Curve::is_supported(curve)) {
        RNP_LOG("Curve %s is not supported.", curv_desc->pgp_name);
        return nullptr;
    }
    int nid = OBJ_sn2nid(curv_desc->openssl_name);
    if (nid == NID_undef) {
        /* LCOV_EXCL_START */
        RNP_LOG("Unknown SN: %s", curv_desc->openssl_name);
        return nullptr;
        /* LCOV_EXCL_END */
    }
    /* EdDSA and X25519 keys are loaded in a different way */
    if (is_raw_key(curve)) {
        return load_raw_key(keyp, keyx, nid);
    }
#if defined(CRYPTO_BACKEND_OPENSSL3)
    return load_key_openssl3(keyp, keyx, *curv_desc);
#else
    rnp::ossl::ECKey ec(EC_KEY_new_by_curve_name(nid));
    if (!ec) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to create EC key with group %s: %s",
                curv_desc->openssl_name,
                rnp::ossl::latest_err());
        return nullptr;
        /* LCOV_EXCL_END */
    }

    auto               group = EC_KEY_get0_group(ec.get());
    rnp::ossl::ECPoint p(EC_POINT_new(group));
    if (!p) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to allocate point: %lu", ERR_peek_last_error());
        return nullptr;
        /* LCOV_EXCL_END */
    }
    if (EC_POINT_oct2point(group, p.get(), keyp.data(), keyp.size(), NULL) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to decode point: %lu", ERR_peek_last_error());
        return nullptr;
        /* LCOV_EXCL_END */
    }
    if (EC_KEY_set_public_key(ec.get(), p.get()) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to set public key: %lu", ERR_peek_last_error());
        return nullptr;
        /* LCOV_EXCL_END */
    }

    rnp::ossl::evp::PKey pkey(EVP_PKEY_new());
    if (!pkey) {
        /* LCOV_EXCL_START */
        RNP_LOG("EVP_PKEY allocation failed: %lu", ERR_peek_last_error());
        return nullptr;
        /* LCOV_EXCL_END */
    }

    if (EVP_PKEY_set1_EC_KEY(pkey.get(), ec.get()) <= 0) {
        return nullptr;
    }

    if (!keyx) {
        return pkey;
    }

    rnp::bn x(keyx);
    if (!x) {
        /* LCOV_EXCL_START */
        RNP_LOG("allocation failed");
        return nullptr;
        /* LCOV_EXCL_END */
    }
    if (EC_KEY_set_private_key(ec.get(), x.get()) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to set secret key: %lu", ERR_peek_last_error());
        return nullptr;
        /* LCOV_EXCL_END */
    }
    return pkey;
#endif
}

rnp_result_t
validate_key(const Key &key, bool secret)
{
    if (key.curve == PGP_CURVE_25519) {
        /* No key check implementation for x25519 in the OpenSSL yet, so just basic size checks
         */
        if ((key.p.size() != 33) || (key.p[0] != 0x40)) {
            return RNP_ERROR_BAD_PARAMETERS;
        }
        if (secret && key.x.size() != 32) {
            return RNP_ERROR_BAD_PARAMETERS;
        }
        return RNP_SUCCESS;
    }
    auto evpkey = load_key(key.p, secret ? &key.x : NULL, key.curve);
    if (!evpkey) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    rnp::ossl::evp::PKeyCtx ctx(EVP_PKEY_CTX_new(evpkey.get(), NULL));
    if (!ctx) {
        /* LCOV_EXCL_START */
        RNP_LOG("Context allocation failed: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    int res = secret ? EVP_PKEY_check(ctx.get()) : EVP_PKEY_public_check(ctx.get());
    if (res < 0) {
        /* LCOV_EXCL_START */
        auto err = ERR_peek_last_error();
        RNP_LOG("EC key check failed: %lu (%s)", err, ERR_reason_error_string(err));
        /* LCOV_EXCL_END */
    }
    if (res <= 0) {
        return RNP_ERROR_GENERIC;
    }
    return RNP_SUCCESS;
}

bool
write_pubkey(const rnp::ossl::evp::PKey &pkey, mpi &mpi, pgp_curve_t curve)
{
    if (is_raw_key(curve)) {
        /* EdDSA and X25519 keys are saved in a different way */
        mpi.resize(33);
        size_t mlen = mpi.size() - 1;
        if (EVP_PKEY_get_raw_public_key(pkey.get(), &mpi[1], &mlen) <= 0) {
            /* LCOV_EXCL_START */
            RNP_LOG("Failed get raw public key: %lu", ERR_peek_last_error());
            return false;
            /* LCOV_EXCL_END */
        }
        assert(mlen == 32);
        mpi[0] = 0x40;
        return true;
    }
    auto ec_desc = Curve::get(curve);
    if (!ec_desc) {
        return false;
    }
    size_t flen = ec_desc->bytes();
#if defined(CRYPTO_BACKEND_OPENSSL3)
    rnp::bn qx, qy;
    /* OpenSSL before 3.0.9 by default uses compressed point for OSSL_PKEY_PARAM_PUB_KEY so use
     * this approach */
    if (!EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_EC_PUB_X, qx.ptr()) ||
        !EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_EC_PUB_Y, qy.ptr())) {
        return false;
    }
    /* Compose uncompressed point in mpi */
    size_t xlen = qx.bytes();
    size_t ylen = qy.bytes();
    assert((xlen <= flen) && (ylen <= flen));
    mpi.resize(2 * flen + 1);
    mpi[0] = 0x04;
    return qx.bin(&mpi[1 + flen - xlen]) && qy.bin(&mpi[1 + 2 * flen - ylen]);
#else
    auto ec = EVP_PKEY_get0_EC_KEY(pkey.get());
    if (!ec) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to retrieve EC key: %lu", ERR_peek_last_error());
        return false;
        /* LCOV_EXCL_END */
    }
    auto p = EC_KEY_get0_public_key(ec);
    if (!p) {
        /* LCOV_EXCL_START */
        RNP_LOG("Null point: %lu", ERR_peek_last_error());
        return false;
        /* LCOV_EXCL_END */
    }
    /* call below adds leading zeroes if needed */
    mpi.resize(2 * flen + 1);
    size_t mlen = EC_POINT_point2oct(
      EC_KEY_get0_group(ec), p, POINT_CONVERSION_UNCOMPRESSED, mpi.data(), mpi.size(), NULL);
    if (!mlen) {
        RNP_LOG("Failed to encode public key: %lu", ERR_peek_last_error()); // LCOV_EXCL_LINE
    }
    mpi.resize(mlen);
    return true;
#endif
}

} // namespace ec
} // namespace pgp
