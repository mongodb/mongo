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

#include <openssl/evp.h>
#include <stddef.h>

#include "crypto/s2n_pq.h"
#include "error/s2n_errno.h"
#include "tls/s2n_kem.h"
#include "utils/s2n_safety.h"
#include "utils/s2n_safety_macros.h"

#if defined(S2N_LIBCRYPTO_SUPPORTS_EVP_KEM)

DEFINE_POINTER_CLEANUP_FUNC(EVP_PKEY *, EVP_PKEY_free);
DEFINE_POINTER_CLEANUP_FUNC(EVP_PKEY_CTX *, EVP_PKEY_CTX_free);

int s2n_evp_kem_generate_keypair(IN const struct s2n_kem *kem, OUT uint8_t *public_key,
        OUT uint8_t *secret_key)
{
    POSIX_ENSURE_REF(kem);
    POSIX_ENSURE(kem->kem_nid != NID_undef, S2N_ERR_UNIMPLEMENTED);
    DEFER_CLEANUP(EVP_PKEY_CTX *kem_pkey_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_KEM, NULL), EVP_PKEY_CTX_free_pointer);
    POSIX_ENSURE_REF(kem_pkey_ctx);
    POSIX_GUARD_OSSL(EVP_PKEY_CTX_kem_set_params(kem_pkey_ctx, kem->kem_nid), S2N_ERR_PQ_CRYPTO);
    POSIX_GUARD_OSSL(EVP_PKEY_keygen_init(kem_pkey_ctx), S2N_ERR_PQ_CRYPTO);

    DEFER_CLEANUP(EVP_PKEY *kem_pkey = NULL, EVP_PKEY_free_pointer);
    POSIX_GUARD_OSSL(EVP_PKEY_keygen(kem_pkey_ctx, &kem_pkey), S2N_ERR_PQ_CRYPTO);
    POSIX_ENSURE_REF(kem_pkey);

    size_t public_key_size = kem->public_key_length;
    POSIX_GUARD_OSSL(EVP_PKEY_get_raw_public_key(kem_pkey, public_key, &public_key_size), S2N_ERR_PQ_CRYPTO);
    POSIX_ENSURE_EQ(kem->public_key_length, public_key_size);
    size_t private_key_size = kem->private_key_length;
    POSIX_GUARD_OSSL(EVP_PKEY_get_raw_private_key(kem_pkey, secret_key, &private_key_size), S2N_ERR_PQ_CRYPTO);
    POSIX_ENSURE_EQ(kem->private_key_length, private_key_size);

    return S2N_SUCCESS;
}

int s2n_evp_kem_encapsulate(IN const struct s2n_kem *kem, OUT uint8_t *ciphertext, OUT uint8_t *shared_secret,
        IN const uint8_t *public_key)
{
    POSIX_ENSURE_REF(kem);
    POSIX_ENSURE(kem->kem_nid != NID_undef, S2N_ERR_UNIMPLEMENTED);
    DEFER_CLEANUP(EVP_PKEY *kem_pkey = EVP_PKEY_kem_new_raw_public_key(kem->kem_nid, public_key, kem->public_key_length), EVP_PKEY_free_pointer);
    POSIX_ENSURE_REF(kem_pkey);

    DEFER_CLEANUP(EVP_PKEY_CTX *kem_pkey_ctx = EVP_PKEY_CTX_new(kem_pkey, NULL), EVP_PKEY_CTX_free_pointer);
    POSIX_ENSURE_REF(kem_pkey_ctx);

    size_t ciphertext_size = kem->ciphertext_length;
    size_t shared_secret_size = kem->shared_secret_key_length;
    POSIX_GUARD_OSSL(EVP_PKEY_encapsulate(kem_pkey_ctx, ciphertext, &ciphertext_size, shared_secret,
                             &shared_secret_size),
            S2N_ERR_PQ_CRYPTO);
    POSIX_ENSURE_EQ(kem->ciphertext_length, ciphertext_size);
    POSIX_ENSURE_EQ(kem->shared_secret_key_length, shared_secret_size);

    return S2N_SUCCESS;
}

int s2n_evp_kem_decapsulate(IN const struct s2n_kem *kem, OUT uint8_t *shared_secret, IN const uint8_t *ciphertext,
        IN const uint8_t *private_key)
{
    POSIX_ENSURE_REF(kem);
    POSIX_ENSURE(kem->kem_nid != NID_undef, S2N_ERR_UNIMPLEMENTED);
    DEFER_CLEANUP(EVP_PKEY *kem_pkey = EVP_PKEY_kem_new_raw_secret_key(kem->kem_nid, private_key, kem->private_key_length), EVP_PKEY_free_pointer);
    POSIX_ENSURE_REF(kem_pkey);

    DEFER_CLEANUP(EVP_PKEY_CTX *kem_pkey_ctx = EVP_PKEY_CTX_new(kem_pkey, NULL), EVP_PKEY_CTX_free_pointer);
    POSIX_ENSURE_REF(kem_pkey_ctx);

    size_t shared_secret_size = kem->shared_secret_key_length;
    POSIX_GUARD_OSSL(EVP_PKEY_decapsulate(kem_pkey_ctx, shared_secret, &shared_secret_size,
                             (uint8_t *) ciphertext, kem->ciphertext_length),
            S2N_ERR_PQ_CRYPTO);
    POSIX_ENSURE_EQ(kem->shared_secret_key_length, shared_secret_size);

    return S2N_SUCCESS;
}

#else /* If !S2N_LIBCRYPTO_SUPPORTS_EVP_KEM, we won't have a kem impl so define relevant stubs here. */

int s2n_evp_kem_generate_keypair(IN const struct s2n_kem *kem, OUT uint8_t *public_key,
        OUT uint8_t *private_key)
{
    POSIX_BAIL(S2N_ERR_UNIMPLEMENTED);
}

int s2n_evp_kem_encapsulate(IN const struct s2n_kem *kem, OUT uint8_t *ciphertext, OUT uint8_t *shared_secret,
        IN const uint8_t *public_key)
{
    POSIX_BAIL(S2N_ERR_UNIMPLEMENTED);
}

int s2n_evp_kem_decapsulate(IN const struct s2n_kem *kem, OUT uint8_t *shared_secret, IN const uint8_t *ciphertext,
        IN const uint8_t *private_key)
{
    POSIX_BAIL(S2N_ERR_UNIMPLEMENTED);
}

#endif
