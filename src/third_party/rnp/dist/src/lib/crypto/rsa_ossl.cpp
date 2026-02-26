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
#include <cstring>
#include <cassert>
#include "crypto/rsa.h"
#include "config.h"
#include "utils.h"
#include "ossl_utils.hpp"
#include <openssl/rsa.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#ifdef CRYPTO_BACKEND_OPENSSL3
#include <openssl/param_build.h>
#include <openssl/core_names.h>
#endif
#include "hash_ossl.hpp"

namespace pgp {
namespace rsa {

#if !defined(CRYPTO_BACKEND_OPENSSL3)
static rnp::ossl::evp::PKey
load_public_key(const Key &key)
{
    rnp::bn        n(key.n);
    rnp::bn        e(key.e);
    rnp::ossl::RSA rsa(RSA_new());

    if (!n || !e || !rsa) {
        /* LCOV_EXCL_START */
        RNP_LOG("out of memory");
        return nullptr;
        /* LCOV_EXCL_END */
    }
    /* OpenSSL set0 function transfers ownership of bignums */
    if (RSA_set0_key(rsa.get(), n.own(), e.own(), NULL) != 1) {
        /* LCOV_EXCL_START */
        RNP_LOG("Public key load error: %lu", ERR_peek_last_error());
        return nullptr;
        /* LCOV_EXCL_END */
    }

    rnp::ossl::evp::PKey evpkey(EVP_PKEY_new());
    if (!evpkey || (EVP_PKEY_set1_RSA(evpkey.get(), rsa.get()) <= 0)) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to set key: %lu", ERR_peek_last_error());
        return nullptr;
        /* LCOV_EXCL_END */
    }
    return evpkey;
}

static rnp::ossl::evp::PKey
load_secret_key(const Key &key)
{
    rnp::bn        n(key.n);
    rnp::bn        e(key.e);
    rnp::bn        p(key.p);
    rnp::bn        q(key.q);
    rnp::bn        d(key.d);
    rnp::ossl::RSA rsa(RSA_new());

    if (!n || !p || !q || !e || !d || !rsa) {
        /* LCOV_EXCL_START */
        RNP_LOG("out of memory");
        return nullptr;
        /* LCOV_EXCL_END */
    }

    /* OpenSSL set0 function transfers ownership of bignums */
    if (RSA_set0_key(rsa.get(), n.own(), e.own(), d.own()) != 1) {
        /* LCOV_EXCL_START */
        RNP_LOG("Secret key load error: %lu", ERR_peek_last_error());
        return nullptr;
        /* LCOV_EXCL_END */
    }
    /* OpenSSL has p < q, as we do */
    if (RSA_set0_factors(rsa.get(), p.own(), q.own()) != 1) {
        /* LCOV_EXCL_START */
        RNP_LOG("Factors load error: %lu", ERR_peek_last_error());
        return nullptr;
        /* LCOV_EXCL_END */
    }

    rnp::ossl::evp::PKey evpkey(EVP_PKEY_new());
    if (!evpkey || (EVP_PKEY_set1_RSA(evpkey.get(), rsa.get()) <= 0)) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to set key: %lu", ERR_peek_last_error());
        return nullptr;
        /* LCOV_EXCL_END */
    }
    return evpkey;
}

static rnp::ossl::evp::PKeyCtx
init_context(const Key &key, bool secret)
{
    auto evpkey = secret ? load_secret_key(key) : load_public_key(key);
    if (!evpkey) {
        return rnp::ossl::evp::PKeyCtx(); // LCOV_EXCL_LINE
    }
    rnp::ossl::evp::PKeyCtx ctx(EVP_PKEY_CTX_new(evpkey.get(), NULL));
    if (!ctx) {
        RNP_LOG("Context allocation failed: %lu", ERR_peek_last_error()); // LCOV_EXCL_LINE
    }
    return ctx;
}
#else
static rnp::ossl::Param
bld_params(const Key &key, bool secret)
{
    rnp::ossl::ParamBld bld(OSSL_PARAM_BLD_new());
    rnp::bn             n(key.n);
    rnp::bn             e(key.e);

    if (!n || !e || !bld) {
        /* LCOV_EXCL_START */
        RNP_LOG("Out of memory");
        return nullptr;
        /* LCOV_EXCL_END */
    }

    if (!OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_N, n.get()) ||
        !OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_E, e.get())) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to push RSA params.");
        return nullptr;
        /* LCOV_EXCL_END */
    }

    if (!secret) {
        auto params = rnp::ossl::Param(OSSL_PARAM_BLD_to_param(bld.get()));
        if (!params) {
            /* LCOV_EXCL_START */
            RNP_LOG("Failed to build RSA pub params: %s.", rnp::ossl::latest_err());
            /* LCOV_EXCL_END */
        }
        return params;
    }

    /* Add secret key fields */
    rnp::bn d(key.d);
    /* As we have u = p^-1 mod q, and qInv = q^-1 mod p, we need to replace one with another */
    rnp::bn p(key.q);
    rnp::bn q(key.p);
    rnp::bn u(key.u);

    if (!d || !p || !q || !u) {
        return nullptr; // LCOV_EXCL_LINE
    }
    /* We need to calculate exponents manually */
    rnp::ossl::BNCtx bnctx(BN_CTX_new());
    if (!bnctx) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to allocate BN_CTX.");
        return nullptr;
        /* LCOV_EXCL_END */
    }
    auto p1 = BN_CTX_get(bnctx.get());
    auto q1 = BN_CTX_get(bnctx.get());
    auto dp = BN_CTX_get(bnctx.get());
    auto dq = BN_CTX_get(bnctx.get());
    if (!BN_copy(p1, p.get()) || !BN_sub_word(p1, 1) || !BN_copy(q1, q.get()) ||
        !BN_sub_word(q1, 1) || !BN_mod(dp, d.get(), p1, bnctx.get()) ||
        !BN_mod(dq, d.get(), q1, bnctx.get())) {
        RNP_LOG("Failed to calculate dP or dQ."); // LCOV_EXCL_LINE
    }
    /* Push params */
    if (!OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_D, d.get()) ||
        !OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_FACTOR1, p.get()) ||
        !OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_FACTOR2, q.get()) ||
        !OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_EXPONENT1, dp) ||
        !OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_EXPONENT2, dq) ||
        !OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_COEFFICIENT1, u.get())) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to push RSA secret params.");
        return nullptr;
        /* LCOV_EXCL_END */
    }
    auto params = rnp::ossl::Param(OSSL_PARAM_BLD_to_param(bld.get()));
    if (!params) {
        RNP_LOG("Failed to build RSA params: %s.", rnp::ossl::latest_err()); // LCOV_EXCL_LINE
    }
    return params;
}

static rnp::ossl::evp::PKey
load_key(const Key &key, bool secret)
{
    /* Build params */
    auto params = bld_params(key, secret);
    if (!params) {
        return nullptr; // LCOV_EXCL_LINE
    }
    /* Create context for key creation */
    rnp::ossl::evp::PKeyCtx ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL));
    if (!ctx) {
        /* LCOV_EXCL_START */
        RNP_LOG("Context allocation failed: %s", rnp::ossl::latest_err());
        return nullptr;
        /* LCOV_EXCL_END */
    }
    /* Create key */
    if (EVP_PKEY_fromdata_init(ctx.get()) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to initialize key creation: %s", rnp::ossl::latest_err());
        return nullptr;
        /* LCOV_EXCL_END */
    }
    EVP_PKEY *res = NULL;
    int       sel = secret ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY;
    if (EVP_PKEY_fromdata(ctx.get(), &res, sel, params.get()) <= 0) {
        RNP_LOG("Failed to create RSA key: %s", rnp::ossl::latest_err()); // LCOV_EXCL_LINE
    }
    return rnp::ossl::evp::PKey(res);
}

static rnp::ossl::evp::PKeyCtx
init_context(const Key &key, bool secret)
{
    auto pkey = load_key(key, secret);
    if (!pkey) {
        return rnp::ossl::evp::PKeyCtx(); // LCOV_EXCL_LINE
    }
    rnp::ossl::evp::PKeyCtx ctx(EVP_PKEY_CTX_new(pkey.get(), NULL));
    if (!ctx) {
        RNP_LOG("Context allocation failed: %s", rnp::ossl::latest_err()); // LCOV_EXCL_LINE
    }
    return ctx;
}
#endif

rnp_result_t
Key::validate(rnp::RNG &rng, bool secret) const noexcept
{
#if defined(CRYPTO_BACKEND_OPENSSL3)
    auto ctx = init_context(*this, secret);
    if (!ctx) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to init context: %s", rnp::ossl::latest_err());
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    int res = secret ? EVP_PKEY_pairwise_check(ctx.get()) : EVP_PKEY_public_check(ctx.get());
    if (res <= 0) {
        RNP_LOG("Key validation error: %s", rnp::ossl::latest_err()); // LCOV_EXCL_LINE
    }
    return res > 0 ? RNP_SUCCESS : RNP_ERROR_GENERIC;
#else
    if (secret) {
        auto ctx = init_context(*this, secret);
        if (!ctx) {
            /* LCOV_EXCL_START */
            RNP_LOG("Failed to init context: %s", rnp::ossl::latest_err());
            return RNP_ERROR_GENERIC;
            /* LCOV_EXCL_END */
        }
        int res = EVP_PKEY_check(ctx.get());
        if (res <= 0) {
            RNP_LOG("Key validation error: %s", rnp::ossl::latest_err()); // LCOV_EXCL_LINE
        }
        return res > 0 ? RNP_SUCCESS : RNP_ERROR_GENERIC;
    }

    /* OpenSSL 1.1.1 doesn't have RSA public key check function, so let's do some checks */
    rnp::bn on(n);
    rnp::bn oe(e);
    if (!on || !oe) {
        /* LCOV_EXCL_START */
        RNP_LOG("out of memory");
        return RNP_ERROR_OUT_OF_MEMORY;
        /* LCOV_EXCL_END */
    }
    if ((BN_num_bits(on.get()) < 512) || !BN_is_odd(on.get()) || (BN_num_bits(oe.get()) < 2) ||
        !BN_is_odd(oe.get())) {
        return RNP_ERROR_GENERIC;
    }
    return RNP_SUCCESS;
#endif
}

static bool
setup_context(rnp::ossl::evp::PKeyCtx &ctx)
{
    if (EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_PADDING) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to set padding: %lu", ERR_peek_last_error());
        return false;
        /* LCOV_EXCL_END */
    }
    return true;
}

static const uint8_t PKCS1_SHA1_ENCODING[15] = {
  0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14};

static bool
setup_signature_hash(rnp::ossl::evp::PKeyCtx &ctx,
                     pgp_hash_alg_t           hash_alg,
                     rnp::secure_bytes &      enc)
{
    auto hash_name = rnp::Hash_OpenSSL::name(hash_alg);
    if (!hash_name) {
        RNP_LOG("Unknown hash: %d", (int) hash_alg);
        return false;
    }
    auto hash_tp = EVP_get_digestbyname(hash_name);
    if (!hash_tp) {
        /* LCOV_EXCL_START */
        RNP_LOG("Error creating hash object for '%s'", hash_name);
        return false;
        /* LCOV_EXCL_END */
    }
    if (EVP_PKEY_CTX_set_signature_md(ctx.get(), hash_tp) <= 0) {
        if ((hash_alg != PGP_HASH_SHA1)) {
            RNP_LOG("Failed to set digest %s: %s", hash_name, rnp::ossl::latest_err());
            return false;
        }
        enc.assign(PKCS1_SHA1_ENCODING, PKCS1_SHA1_ENCODING + sizeof(PKCS1_SHA1_ENCODING));
    } else {
        enc.resize(0);
    }
    return true;
}

rnp_result_t
Key::encrypt_pkcs1(rnp::RNG &rng, Encrypted &out, const rnp::secure_bytes &in) const noexcept
{
    auto ctx = init_context(*this, false);
    if (!ctx) {
        return RNP_ERROR_GENERIC; // LCOV_EXCL_LINE
    }
    if (EVP_PKEY_encrypt_init(ctx.get()) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to initialize encryption: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    if (!setup_context(ctx)) {
        return RNP_ERROR_GENERIC; // LCOV_EXCL_LINE
    }
    out.m.resize(n.size());
    size_t mlen = out.m.size();
    if (EVP_PKEY_encrypt(ctx.get(), out.m.data(), &mlen, in.data(), in.size()) <= 0) {
        RNP_LOG("Encryption failed: %lu", ERR_peek_last_error());
        out.m.resize(0);
        return RNP_ERROR_GENERIC;
    }
    out.m.resize(mlen);
    return RNP_SUCCESS;
}

rnp_result_t
Key::verify_pkcs1(const Signature &        sig,
                  pgp_hash_alg_t           hash_alg,
                  const rnp::secure_bytes &hash) const noexcept
{
    auto ctx = init_context(*this, false);
    if (!ctx) {
        return RNP_ERROR_SIGNATURE_INVALID; // LCOV_EXCL_LINE
    }

    if (EVP_PKEY_verify_init(ctx.get()) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to initialize verification: %lu", ERR_peek_last_error());
        return RNP_ERROR_SIGNATURE_INVALID;
        /* LCOV_EXCL_END */
    }

    rnp::secure_bytes hash_enc;
    if (!setup_context(ctx) || !setup_signature_hash(ctx, hash_alg, hash_enc)) {
        return RNP_ERROR_SIGNATURE_INVALID; // LCOV_EXCL_LINE
    }
    /* Check whether we need to workaround on unsupported SHA1 for RSA signature verification
     */
    auto hptr = hash.data();
    auto hsize = hash.size();
    if (!hash_enc.empty()) {
        hash_enc.insert(hash_enc.end(), hash.begin(), hash.end());
        hptr = hash_enc.data();
        hsize = hash_enc.size();
    }
    int res = 0;
    if (sig.s.size() < n.size()) {
        /* OpenSSL doesn't like signatures smaller then N */
        std::vector<uint8_t> sn(n.size() - sig.s.size(), 0);
        sn.insert(sn.end(), sig.s.data(), sig.s.data() + sig.s.size());
        res = EVP_PKEY_verify(ctx.get(), sn.data(), sn.size(), hptr, hsize);
    } else {
        res = EVP_PKEY_verify(ctx.get(), sig.s.data(), sig.s.size(), hptr, hsize);
    }
    if (res <= 0) {
        RNP_LOG("RSA verification failure: %s", rnp::ossl::latest_err());
        return RNP_ERROR_SIGNATURE_INVALID;
    }
    return RNP_SUCCESS;
}

rnp_result_t
Key::sign_pkcs1(rnp::RNG &               rng,
                Signature &              sig,
                pgp_hash_alg_t           hash_alg,
                const rnp::secure_bytes &hash) const noexcept
{
    if (!q.size()) {
        /* LCOV_EXCL_START */
        RNP_LOG("private key not set");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    auto ctx = init_context(*this, true);
    if (!ctx) {
        return RNP_ERROR_GENERIC; // LCOV_EXCL_LINE
    }

    if (EVP_PKEY_sign_init(ctx.get()) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to initialize signing: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    rnp::secure_bytes hash_enc;
    if (!setup_context(ctx) || !setup_signature_hash(ctx, hash_alg, hash_enc)) {
        return RNP_ERROR_GENERIC; // LCOV_EXCL_LINE
    }
    /* Check whether we need to workaround on unsupported SHA1 for RSA signature verification
     */
    auto hptr = hash.data();
    auto hsize = hash.size();
    if (!hash_enc.empty()) {
        hash_enc.insert(hash_enc.end(), hash.begin(), hash.end());
        hptr = hash_enc.data();
        hsize = hash_enc.size();
    }
    sig.s.resize(n.size());
    size_t slen = sig.s.size();
    if (EVP_PKEY_sign(ctx.get(), sig.s.data(), &slen, hptr, hsize) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Signing failed: %lu", ERR_peek_last_error());
        sig.s.resize(0);
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    return RNP_SUCCESS;
}

rnp_result_t
Key::decrypt_pkcs1(rnp::RNG &rng, rnp::secure_bytes &out, const Encrypted &in) const noexcept
{
    if (!q.size()) {
        /* LCOV_EXCL_START */
        RNP_LOG("private key not set");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    auto ctx = init_context(*this, true);
    if (!ctx) {
        return RNP_ERROR_GENERIC; // LCOV_EXCL_LINE
    }
    if (EVP_PKEY_decrypt_init(ctx.get()) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to initialize encryption: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    if (!setup_context(ctx)) {
        return RNP_ERROR_GENERIC; // LCOV_EXCL_LINE
    }
    out.resize(n.size());
    size_t out_len = out.size();
    if (EVP_PKEY_decrypt(ctx.get(), out.data(), &out_len, in.m.data(), in.m.size()) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Encryption failed: %lu", ERR_peek_last_error());
        out.resize(0);
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    out.resize(out_len);
    return RNP_SUCCESS;
}

static bool
calculate_pqu(const rnp::bn &p, const rnp::bn &q, const rnp::bn &u, Key &key)
{
    /* OpenSSL doesn't care whether p < q */
    if (BN_cmp(p.c_get(), q.c_get()) > 0) {
        /* In this case we have u, as iqmp is inverse of q mod p, and we exchange them */
        return q.mpi(key.p) && p.mpi(key.q) && u.mpi(key.u);
    }

    rnp::ossl::BNCtx bnctx(BN_CTX_new());
    if (!bnctx) {
        return false;
    }

    /* we need to calculate u, since we need inverse of p mod q, while OpenSSL has inverse of q
     * mod p, and doesn't care of p < q */
    BN_CTX_start(bnctx.get());
    auto nu = BN_CTX_get(bnctx.get());
    auto nq = BN_CTX_get(bnctx.get());
    if (!nu || !nq) {
        return false;
    }
    BN_with_flags(nq, q.c_get(), BN_FLG_CONSTTIME);
    /* calculate inverse of p mod q */
    if (!BN_mod_inverse(nu, p.c_get(), nq, bnctx.get())) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to calculate u");
        return false;
        /* LCOV_EXCL_END */
    }
    if (!p.mpi(key.p) || !q.mpi(key.q)) {
        return false;
    }
    return rnp::bn::mpi(nu, key.u);
}

static bool
extract_key(rnp::ossl::evp::PKey &pkey, Key &key)
{
#if defined(CRYPTO_BACKEND_OPENSSL3)
    rnp::bn n, e, d, p, q, u;
    return EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_N, n.ptr()) &&
           EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_E, e.ptr()) &&
           EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_D, d.ptr()) &&
           EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_FACTOR1, p.ptr()) &&
           EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_FACTOR2, q.ptr()) &&
           EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_COEFFICIENT1, u.ptr()) &&
           calculate_pqu(p, q, u, key) && n.mpi(key.n) && e.mpi(key.e) && d.mpi(key.d);
#else
    const RSA *rsa = EVP_PKEY_get0_RSA(pkey.get());
    if (!rsa) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to retrieve RSA key: %lu", ERR_peek_last_error());
        return false;
        /* LCOV_EXCL_END */
    }
    if (RSA_check_key(rsa) != 1) {
        RNP_LOG("Key validation error: %lu", ERR_peek_last_error());
        return false;
    }

    rnp::bn n(RSA_get0_n(rsa));
    rnp::bn e(RSA_get0_e(rsa));
    rnp::bn d(RSA_get0_d(rsa));
    rnp::bn p(RSA_get0_p(rsa));
    rnp::bn q(RSA_get0_q(rsa));
    rnp::bn u(RSA_get0_iqmp(rsa));
    if (!n || !e || !d || !p || !q || !u) {
        return false;
    }
    if (!calculate_pqu(p, q, u, key)) {
        return false;
    }
    return n.mpi(key.n) && e.mpi(key.e) && d.mpi(key.d);
#endif
}

rnp_result_t
Key::generate(rnp::RNG &rng, size_t numbits) noexcept
{
    if ((numbits < 1024) || (numbits > PGP_MPINT_BITS)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    rnp::ossl::evp::PKeyCtx ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL));
    if (!ctx) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to create ctx: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    if (EVP_PKEY_keygen_init(ctx.get()) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to init keygen: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), numbits) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to set rsa bits: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    EVP_PKEY *rawkey = NULL;
    if (EVP_PKEY_keygen(ctx.get(), &rawkey) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("RSA keygen failed: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    rnp::ossl::evp::PKey pkey(rawkey);
    if (!extract_key(pkey, *this)) {
        return RNP_ERROR_GENERIC; // LCOV_EXCL_LINE
    }
    return RNP_SUCCESS;
}

} // namespace rsa
} // namespace pgp
