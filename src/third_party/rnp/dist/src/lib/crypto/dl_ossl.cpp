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
#include <string>
#include <cassert>
#include "dl_ossl.h"
#include "utils.h"
#include "ossl_utils.hpp"
#include <openssl/dh.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#if defined(CRYPTO_BACKEND_OPENSSL3)
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#endif

#if defined(CRYPTO_BACKEND_OPENSSL3)
static rnp::ossl::Param
dl_build_params(
  const rnp::bn &p, const rnp::bn &q, const rnp::bn &g, const rnp::bn &y, const rnp::bn &x)
{
    rnp::ossl::ParamBld bld(OSSL_PARAM_BLD_new());
    if (!bld || !OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_FFC_P, p.c_get()) ||
        (q && !OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_FFC_Q, q.c_get())) ||
        !OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_FFC_G, g.c_get()) ||
        !OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_PUB_KEY, y.c_get()) ||
        (x && !OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_PRIV_KEY, x.c_get()))) {
        return rnp::ossl::Param(); // LCOV_EXCL_LINE
    }
    return rnp::ossl::Param(OSSL_PARAM_BLD_to_param(bld.get()));
}
#endif

rnp::ossl::evp::PKey
dl_load_key(const pgp::mpi &mp,
            const pgp::mpi *mq,
            const pgp::mpi &mg,
            const pgp::mpi &my,
            const pgp::mpi *mx)
{
    rnp::bn p(mp);
    rnp::bn q(mq);
    rnp::bn g(mg);
    rnp::bn y(my);
    rnp::bn x(mx);

    if (!p || (mq && !q) || !g || !y || (mx && !x)) {
        /* LCOV_EXCL_START */
        RNP_LOG("out of memory");
        return nullptr;
        /* LCOV_EXCL_END */
    }

#if defined(CRYPTO_BACKEND_OPENSSL3)
    auto params = dl_build_params(p, q, g, y, x);
    if (!params) {
        /* LCOV_EXCL_START */
        RNP_LOG("failed to build dsa params");
        return nullptr;
        /* LCOV_EXCL_END */
    }
    rnp::ossl::evp::PKeyCtx ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_DH, NULL));
    if (!ctx) {
        /* LCOV_EXCL_START */
        RNP_LOG("failed to create dl context");
        return nullptr;
        /* LCOV_EXCL_END */
    }
    EVP_PKEY *rawkey = NULL;
    int       sel = mx ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY;
    if ((EVP_PKEY_fromdata_init(ctx.get()) != 1) ||
        (EVP_PKEY_fromdata(ctx.get(), &rawkey, sel, params.get()) != 1)) {
        RNP_LOG("failed to create key from data"); // LCOV_EXCL_LINE
    }
    return rnp::ossl::evp::PKey(rawkey);
#else
    rnp::ossl::DH dh(DH_new());
    if (!dh) {
        /* LCOV_EXCL_START */
        RNP_LOG("out of memory");
        return nullptr;
        /* LCOV_EXCL_END */
    }
    /* line below must not fail */
    int res = DH_set0_pqg(dh.get(), p.own(), q.own(), g.own());
    assert(res == 1);
    if (res < 1) {
        return nullptr;
    }
    /* line below must not fail */
    res = DH_set0_key(dh.get(), y.own(), x.own());
    assert(res == 1);
    if (res < 1) {
        return nullptr;
    }

    rnp::ossl::evp::PKey evpkey(EVP_PKEY_new());
    if (!evpkey) {
        /* LCOV_EXCL_START */
        RNP_LOG("allocation failed");
        return nullptr;
        /* LCOV_EXCL_END */
    }
    if (EVP_PKEY_set1_DH(evpkey.get(), dh.get()) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to set key: %lu", ERR_peek_last_error());
        return nullptr;
        /* LCOV_EXCL_END */
    }
    return evpkey;
#endif
}

#if !defined(CRYPTO_BACKEND_OPENSSL3)
static rnp_result_t
dl_validate_secret_key(rnp::ossl::evp::PKey &dlkey, const pgp::mpi &mx)
{
    auto dh = EVP_PKEY_get0_DH(dlkey.get());
    assert(dh);
    const rnp::bn p(DH_get0_p(dh));
    rnp::bn       q(DH_get0_q(dh));
    const rnp::bn g(DH_get0_g(dh));
    const rnp::bn y(DH_get0_pub_key(dh));
    assert(p && g && y);

    rnp::ossl::BNCtx ctx(BN_CTX_new());
    rnp::bn          x(mx);
    rnp::bn          cy(BN_new());

    if (!x || !cy || !ctx) {
        /* LCOV_EXCL_START */
        RNP_LOG("Allocation failed");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    if (!q) {
        /* if q is NULL then group order is (p - 1) / 2 */
        rnp::bn p1(BN_dup(p.c_get()));
        if (!p1) {
            /* LCOV_EXCL_START */
            RNP_LOG("Allocation failed");
            return RNP_ERROR_GENERIC;
            /* LCOV_EXCL_END */
        }
        int res = BN_rshift(p1.get(), p1.get(), 1);
        assert(res == 1);
        if (res < 1) {
            /* LCOV_EXCL_START */
            RNP_LOG("BN_rshift failed.");
            return RNP_ERROR_GENERIC;
            /* LCOV_EXCL_END */
        }
        q = std::move(p1);
    }
    if (BN_cmp(x.get(), q.c_get()) != -1) {
        RNP_LOG("x is too large.");
        return RNP_ERROR_GENERIC;
    }
    BN_CTX_start(ctx.get());
    if (BN_mod_exp_mont_consttime(cy.get(), g.c_get(), x.c_get(), p.c_get(), ctx.get(), NULL) <
        1) {
        RNP_LOG("Exponentiation failed");
        return RNP_ERROR_GENERIC;
    }
    if (BN_cmp(cy.get(), y.c_get()) != 0) {
        return RNP_ERROR_GENERIC;
    }
    return RNP_SUCCESS;
}
#endif

rnp_result_t
dl_validate_key(rnp::ossl::evp::PKey &pkey, const pgp::mpi *x)
{
    rnp::ossl::evp::PKeyCtx ctx(EVP_PKEY_CTX_new(pkey.get(), NULL));
    if (!ctx) {
        /* LCOV_EXCL_START */
        RNP_LOG("Context allocation failed: %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    int res = EVP_PKEY_param_check(ctx.get());
    if (res < 0) {
        RNP_LOG(
          "Param validation error: %lu (%s)", ERR_peek_last_error(), rnp::ossl::latest_err());
    }
    if (res < 1) {
        /* ElGamal specification doesn't seem to restrict P to the safe prime */
        auto err = ERR_peek_last_error();
        DHerr(DH_F_DH_CHECK_EX, DH_R_CHECK_P_NOT_SAFE_PRIME);
        if ((ERR_GET_REASON(err) == DH_R_CHECK_P_NOT_SAFE_PRIME)) {
            RNP_LOG("Warning! P is not a safe prime.");
        } else {
            return RNP_ERROR_GENERIC;
        }
    }
#if defined(CRYPTO_BACKEND_OPENSSL3)
    res = x ? EVP_PKEY_pairwise_check(ctx.get()) : EVP_PKEY_public_check(ctx.get());
    return res == 1 ? RNP_SUCCESS : RNP_ERROR_GENERIC;
#else
    res = EVP_PKEY_public_check(ctx.get());
    if (res < 0) {
        RNP_LOG("Key validation error: %lu", ERR_peek_last_error());
    }
    if (res < 1) {
        return RNP_ERROR_GENERIC;
    }
    /* There is no private key check in OpenSSL yet, so need to check x vs y manually */
    if (!x) {
        return RNP_SUCCESS;
    }
    return dl_validate_secret_key(pkey, *x);
#endif
}
