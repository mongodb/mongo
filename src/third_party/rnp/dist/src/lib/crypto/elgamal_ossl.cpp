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

#include <cstdlib>
#include <cstring>
#include <cassert>
#include <rnp/rnp_def.h>
#include "elgamal.h"
#include "dl_ossl.h"
#include "utils.h"
#include "mem.h"
#include "ossl_utils.hpp"
#include <openssl/bn.h>
#include <openssl/dh.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#if defined(CRYPTO_BACKEND_OPENSSL3)
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#endif

// Max supported key byte size
#define ELGAMAL_MAX_P_BYTELEN BITS_TO_BYTES(PGP_MPINT_BITS)

namespace pgp {
namespace eg {

bool
Key::validate(bool secret) const noexcept
{
    rnp::ossl::BNCtx ctx(BN_CTX_new());
    if (!ctx) {
        /* LCOV_EXCL_START */
        RNP_LOG("Allocation failed.");
        return false;
        /* LCOV_EXCL_END */
    }
    BN_CTX_start(ctx.get());
    rnp::bn op(p);
    rnp::bn og(g);
    auto    p1 = BN_CTX_get(ctx.get());
    auto    r = BN_CTX_get(ctx.get());

    if (!op || !og || !p1 || !r) {
        return false;
    }

    /* 1 < g < p */
    if ((BN_cmp(og.get(), BN_value_one()) != 1) || (BN_cmp(og.get(), op.get()) != -1)) {
        RNP_LOG("Invalid g value.");
        return false;
    }
    /* g ^ (p - 1) = 1 mod p */
    if (!BN_copy(p1, op.get()) || !BN_sub_word(p1, 1) ||
        !BN_mod_exp(r, og.get(), p1, op.get(), ctx.get())) {
        RNP_LOG("g exp failed.");
        return false;
    }
    if (BN_cmp(r, BN_value_one()) != 0) {
        RNP_LOG("Wrong g exp value.");
        return false;
    }
    /* check for small order subgroups */
    rnp::ossl::BNRecpCtx rctx(BN_RECP_CTX_new());
    if (!rctx || !BN_RECP_CTX_set(rctx.get(), op.get(), ctx.get()) || !BN_copy(r, og.get())) {
        RNP_LOG("Failed to init RECP context.");
        return false;
    }
    for (size_t i = 2; i < (1 << 17); i++) {
        if (!BN_mod_mul_reciprocal(r, r, og.get(), rctx.get(), ctx.get())) {
            /* LCOV_EXCL_START */
            RNP_LOG("Multiplication failed.");
            return false;
            /* LCOV_EXCL_END */
        }
        if (BN_cmp(r, BN_value_one()) == 0) {
            RNP_LOG("Small subgroup detected. Order %zu", i);
            return false;
        }
    }
    if (!secret) {
        return true;
    }
    /* check that g ^ x = y (mod p) */
    rnp::bn ox(x);
    rnp::bn oy(y);
    if (!ox || !oy) {
        return false;
    }
    return BN_mod_exp(r, og.get(), ox.get(), op.get(), ctx.get()) && !BN_cmp(r, oy.get());
}

static bool
pkcs1v15_pad(mpi &out, size_t psize, const rnp::secure_bytes &in)
{
    assert(psize >= in.size() + 11);
    out.resize(psize);
    out[0] = 0x00;
    out[1] = 0x02;
    size_t rnd = psize - in.size() - 3;
    out[2 + rnd] = 0x00;
    if (RAND_bytes(&out[2], rnd) != 1) {
        return false;
    }
    for (size_t i = 2; i < 2 + rnd; i++) {
        /* we need non-zero bytes */
        size_t cntr = 16;
        while (!out[i] && (cntr--) && (RAND_bytes(&out[i], 1) == 1)) {
        }
        if (!out[i]) {
            /* LCOV_EXCL_START */
            RNP_LOG("Something is wrong with RNG.");
            return false;
            /* LCOV_EXCL_END */
        }
    }
    memcpy(out.data() + rnd + 3, in.data(), in.size());
    return true;
}

static bool
pkcs1v15_unpad(const mpi &in, rnp::secure_bytes &out, bool skip0)
{
    if (in.size() <= (size_t)(11 - skip0)) {
        return false;
    }
    if (!skip0 && in[0]) {
        return false;
    }
    if (in[1 - skip0] != 0x02) {
        return false;
    }
    size_t pad = 2 - skip0;
    while ((pad < in.size()) && in[pad]) {
        pad++;
    }
    if (pad >= in.size()) {
        return false;
    }
    out.assign(in.data() + pad + 1, in.data() + in.size());
    return true;
}

rnp_result_t
Key::encrypt_pkcs1(rnp::RNG &rng, Encrypted &out, const rnp::secure_bytes &in) const
{
    mpi mm{};
    if (!pkcs1v15_pad(mm, p.size(), in)) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to add PKCS1 v1.5 padding.");
        return RNP_ERROR_BAD_PARAMETERS;
        /* LCOV_EXCL_END */
    }
    rnp::ossl::BNCtx ctx(BN_CTX_new());
    if (!ctx) {
        /* LCOV_EXCL_START */
        RNP_LOG("Allocation failed.");
        return RNP_ERROR_OUT_OF_MEMORY;
        /* LCOV_EXCL_END */
    }
    rnp::bn m(mm);
    rnp::bn op(p);
    rnp::bn og(g);
    rnp::bn oy(y);
    BN_CTX_start(ctx.get());
    auto    c1 = BN_CTX_get(ctx.get());
    auto    c2 = BN_CTX_get(ctx.get());
    rnp::bn k(BN_secure_new());
    if (!m || !op || !og || !oy || !c1 || !c2 || !k) {
        /* LCOV_EXCL_START */
        RNP_LOG("Allocation failed.");
        return RNP_ERROR_OUT_OF_MEMORY;
        /* LCOV_EXCL_END */
    }
    /* initialize Montgomery context */
    rnp::ossl::BNMontCtx mctx(BN_MONT_CTX_new());
    if (!mctx || (BN_MONT_CTX_set(mctx.get(), op.get(), ctx.get()) < 1)) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to setup Montgomery context.");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    /* must not fail */
    int res = BN_rshift1(c1, op.get());
    assert(res == 1);
    if (res < 1) {
        /* LCOV_EXCL_START */
        RNP_LOG("BN_rshift1 failed.");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    /* generate k */
    if (BN_rand_range(k.get(), c1) < 1) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to generate k.");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    /* calculate c1 = g ^ k (mod p) */
    if (BN_mod_exp_mont_consttime(c1, og.get(), k.get(), op.get(), ctx.get(), mctx.get()) <
        1) {
        /* LCOV_EXCL_START */
        RNP_LOG("Exponentiation 1 failed");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    /* calculate c2 = m * y ^ k (mod p)*/
    if (BN_mod_exp_mont_consttime(c2, oy.get(), k.get(), op.get(), ctx.get(), mctx.get()) <
        1) {
        /* LCOV_EXCL_START */
        RNP_LOG("Exponentiation 2 failed");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    if (BN_mod_mul(c2, c2, m.get(), op.get(), ctx.get()) < 1) {
        /* LCOV_EXCL_START */
        RNP_LOG("Multiplication failed");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    res = rnp::bn::mpi(c1, out.g) && rnp::bn::mpi(c2, out.m);
    assert(res == 1);
    return RNP_SUCCESS;
}

rnp_result_t
Key::decrypt_pkcs1(rnp::RNG &rng, rnp::secure_bytes &out, const Encrypted &in) const
{
    if (!x.size()) {
        RNP_LOG("Secret key not set.");
        return RNP_ERROR_BAD_PARAMETERS;
    }
    rnp::ossl::BNCtx ctx(BN_CTX_new());
    if (!ctx) {
        /* LCOV_EXCL_START */
        RNP_LOG("Allocation failed.");
        return RNP_ERROR_OUT_OF_MEMORY;
        /* LCOV_EXCL_END */
    }
    rnp::bn op(p);
    rnp::bn og(g);
    rnp::bn ox(x);
    rnp::bn c1(in.g);
    rnp::bn c2(in.m);
    BN_CTX_start(ctx.get());
    auto    s = BN_CTX_get(ctx.get());
    rnp::bn m(BN_secure_new());
    if (!op || !og || !ox || !c1 || !c2 || !m) {
        /* LCOV_EXCL_START */
        RNP_LOG("Allocation failed.");
        return RNP_ERROR_OUT_OF_MEMORY;
        /* LCOV_EXCL_END */
    }
    /* initialize Montgomery context */
    rnp::ossl::BNMontCtx mctx(BN_MONT_CTX_new());
    if (BN_MONT_CTX_set(mctx.get(), op.get(), ctx.get()) < 1) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to setup Montgomery context.");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    /* calculate s = c1 ^ x (mod p) */
    if (BN_mod_exp_mont_consttime(s, c1.get(), ox.get(), op.get(), ctx.get(), mctx.get()) <
        1) {
        /* LCOV_EXCL_START */
        RNP_LOG("Exponentiation 1 failed");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    /* calculate s^-1 (mod p) */
    BN_set_flags(s, BN_FLG_CONSTTIME);
    if (!BN_mod_inverse(s, s, op.get(), ctx.get())) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to calculate inverse.");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    /* calculate m = c2 * s ^ -1 (mod p)*/
    if (BN_mod_mul(m.get(), c2.get(), s, op.get(), ctx.get()) < 1) {
        /* LCOV_EXCL_START */
        RNP_LOG("Multiplication failed");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    mpi  mm;
    bool res = m.mpi(mm);
    assert(res);
    if (!res) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    /* unpad, handling skipped leftmost 0 case */
    if (!pkcs1v15_unpad(mm, out, mm.size() == p.size() - 1)) {
        RNP_LOG("Unpad failed.");
        return RNP_ERROR_GENERIC;
    }
    mm.forget();
    return RNP_SUCCESS;
}

rnp_result_t
Key::generate(rnp::RNG &rng, size_t keybits)
{
    if ((keybits < 1024) || (keybits > PGP_MPINT_BITS)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* Generate DH params, which usable for ElGamal as well */
    rnp::ossl::evp::PKeyCtx pctx(EVP_PKEY_CTX_new_id(EVP_PKEY_DH, NULL));
    if (!pctx) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to create ctx: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    if (EVP_PKEY_paramgen_init(pctx.get()) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to init keygen: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    if (EVP_PKEY_CTX_set_dh_paramgen_prime_len(pctx.get(), keybits) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to set key bits: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    /* OpenSSL correctly handles case with g = 5, making sure that g is primitive root of
     * q-group */
    if (EVP_PKEY_CTX_set_dh_paramgen_generator(pctx.get(), DH_GENERATOR_5) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to set key generator: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    EVP_PKEY *rparmkey = NULL;
    if (EVP_PKEY_paramgen(pctx.get(), &rparmkey) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to generate parameters: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    rnp::ossl::evp::PKey parmkey(rparmkey);
    /* Generate DH (ElGamal) key */
    do {
        rnp::ossl::evp::PKeyCtx ctx(EVP_PKEY_CTX_new(parmkey.get(), NULL));
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
        EVP_PKEY *rpkey = NULL;
        if (EVP_PKEY_keygen(ctx.get(), &rpkey) <= 0) {
            /* LCOV_EXCL_START */
            RNP_LOG("ElGamal keygen failed: %lu", ERR_peek_last_error());
            return RNP_ERROR_GENERIC;
            /* LCOV_EXCL_END */
        }
        rnp::ossl::evp::PKey pkey(rpkey);
#if defined(CRYPTO_BACKEND_OPENSSL3)
        rnp::bn oy;
        if (!EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_PUB_KEY, oy.ptr())) {
            /* LCOV_EXCL_START */
            RNP_LOG("Failed to retrieve ElGamal public key: %lu", ERR_peek_last_error());
            return RNP_ERROR_GENERIC;
            /* LCOV_EXCL_END */
        }
        if (oy.bytes() != BITS_TO_BYTES(keybits)) {
            /* This code chunk is rarely hit, so ignoring it for the coverage report: */
            continue; // LCOV_EXCL_LINE
        }

        rnp::bn op, og, ox;
        bool    res = EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_FFC_P, op.ptr()) &&
                   EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_FFC_G, og.ptr()) &&
                   EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_PRIV_KEY, ox.ptr()) &&
                   op.mpi(p) && og.mpi(g) && oy.mpi(y) && ox.mpi(x);
        return res ? RNP_SUCCESS : RNP_ERROR_GENERIC;
#else
        auto dh = EVP_PKEY_get0_DH(pkey.get());
        if (!dh) {
            /* LCOV_EXCL_START */
            RNP_LOG("Failed to retrieve DH key: %lu", ERR_peek_last_error());
            return RNP_ERROR_GENERIC;
            /* LCOV_EXCL_END */
        }
        if (BITS_TO_BYTES(BN_num_bits(DH_get0_pub_key(dh))) != BITS_TO_BYTES((int) keybits)) {
            /* This code chunk is rarely hit, so ignoring it for the coverage report */
            continue; // LCOV_EXCL_LINE
        }

        rnp::bn op(DH_get0_p(dh));
        rnp::bn og(DH_get0_g(dh));
        rnp::bn oy(DH_get0_pub_key(dh));
        rnp::bn ox(DH_get0_priv_key(dh));
        if (!op || !og || !oy || !ox) {
            return RNP_ERROR_BAD_STATE; // LCOV_EXCL_START
        }
        op.mpi(p);
        og.mpi(g);
        oy.mpi(y);
        ox.mpi(x);
        return RNP_SUCCESS;
#endif
    } while (1);
}

} // namespace eg
} // namespace pgp
