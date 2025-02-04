/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#pragma once

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "crypto/s2n_openssl.h"
#include "utils/s2n_result.h"

struct s2n_evp_digest {
    const EVP_MD *md;
    EVP_MD_CTX *ctx;
};

struct s2n_evp_hmac_state {
    struct s2n_evp_digest evp_digest;
    union {
        HMAC_CTX *hmac_ctx;
        EVP_PKEY *evp_pkey;
    } ctx;
};

/* Define API's that change based on the OpenSSL Major Version. */
#if S2N_OPENSSL_VERSION_AT_LEAST(1, 1, 0) && !defined(LIBRESSL_VERSION_NUMBER)
    #define S2N_EVP_MD_CTX_NEW()         (EVP_MD_CTX_new())
    #define S2N_EVP_MD_CTX_RESET(md_ctx) (EVP_MD_CTX_reset(md_ctx))
    #define S2N_EVP_MD_CTX_FREE(md_ctx)  (EVP_MD_CTX_free(md_ctx))
#else
    #define S2N_EVP_MD_CTX_NEW()         (EVP_MD_CTX_create())
    #define S2N_EVP_MD_CTX_RESET(md_ctx) (EVP_MD_CTX_cleanup(md_ctx))
    #define S2N_EVP_MD_CTX_FREE(md_ctx)  (EVP_MD_CTX_destroy(md_ctx))
#endif

/* On some versions of OpenSSL, "EVP_PKEY_CTX_set_signature_md()" is just a macro that casts digest_alg to "void*",
 * which fails to compile when the "-Werror=cast-qual" compiler flag is enabled. So we work around this OpenSSL
 * issue by turning off this compiler check for this one function with a cast through.
 */
#define S2N_EVP_PKEY_CTX_set_signature_md(ctx, md) \
    EVP_PKEY_CTX_set_signature_md(ctx, (EVP_MD *) (uintptr_t) md)

int s2n_digest_allow_md5_for_fips(struct s2n_evp_digest *evp_digest);
S2N_RESULT s2n_digest_is_md5_allowed_for_fips(struct s2n_evp_digest *evp_digest, bool *out);
