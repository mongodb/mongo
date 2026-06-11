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

#include "crypto/s2n_mldsa.h"

#include "crypto/s2n_hash.h"
#include "utils/s2n_safety.h"

bool s2n_mldsa_is_supported()
{
#if S2N_LIBCRYPTO_SUPPORTS_MLDSA
    return s2n_hash_supports_shake();
#else
    return false;
#endif
}

/*
 * TLS uses pure ML-DSA, as opposed to pre-hash ML-DSA. However, pure ML-DSA
 * still supports a form of pre-hashing referred to as "external mu".
 *
 * "ExternalMu-ML-DSA" is defined in Appendix D of the ML-DSA PKI RFC:
 * https://www.ietf.org/archive/id/draft-ietf-lamps-dilithium-certificates-07.html#appendix-D
 *
 * However, the AWS-LC codebase includes a much clearer description:
 * https://github.com/aws/aws-lc/blob/07e2e1e9ccce0a1101f14e453dbdb1304c2f3472/crypto/fipsmodule/evp/p_pqdsa.c#L172-L177
 *
 * So in summary:
 * mu = SHAKE256(SHAKE256(pk, 64) || 0 || ctx_len || ctx || M, 64)
 * where:
 *  pk is the raw bytes of the public key.
 *  0 represents the "mode" of pure ML-DSA, as opposed to pre-hash ML-DSA.
 *  ctx_len is the length of the context, which is zero for TLS.
 *  ctx is the context, which is zero-length for TLS.
 *  M is the data to be hashed.
 *  64 is the length of the SHAKE256 digest.
 */
#define S2N_MLDSA_DIGEST_LENGTH 64
const uint8_t mode_and_ctx[] = { 0, 0 };
S2N_RESULT s2n_mldsa_init_mu_hash(struct s2n_hash_state *state, const struct s2n_pkey *pub_key)
{
    RESULT_ENSURE_REF(state);
    RESULT_ENSURE_REF(pub_key);
    RESULT_ENSURE_REF(pub_key->pkey);

    /* The required prefix must be the first data added to the hash */
    uint64_t currently_in_hash = 0;
    RESULT_GUARD_POSIX(s2n_hash_get_currently_in_hash_total(state, &currently_in_hash));
    RESULT_ENSURE(currently_in_hash == 0, S2N_ERR_HASH_NOT_READY);

    /* Get the raw bytes of the public key */
    uint8_t public_key_bytes[S2N_MLDSA_MAX_PUB_KEY_SIZE] = { 0 };
    size_t public_key_size = sizeof(public_key_bytes);
#if S2N_LIBCRYPTO_SUPPORTS_MLDSA
    RESULT_GUARD_OSSL(EVP_PKEY_get_raw_public_key(pub_key->pkey, public_key_bytes, &public_key_size),
            S2N_ERR_HASH_INIT_FAILED);
#else
    RESULT_BAIL(S2N_ERR_INVALID_SIGNATURE_ALGORITHM);
#endif

    /* Get the digest of the raw bytes of the public key.
     * We can use the current hash state. We'll reset it afterwards. */
    uint8_t public_key_digest[S2N_MLDSA_DIGEST_LENGTH] = { 0 };
    RESULT_GUARD_POSIX(s2n_hash_update(state, public_key_bytes, public_key_size));
    RESULT_GUARD_POSIX(s2n_hash_digest(state, public_key_digest, S2N_MLDSA_DIGEST_LENGTH));
    RESULT_GUARD_POSIX(s2n_hash_reset(state));

    /* Add all the required prefix data */
    RESULT_GUARD_POSIX(s2n_hash_update(state, public_key_digest, S2N_MLDSA_DIGEST_LENGTH));
    RESULT_GUARD_POSIX(s2n_hash_update(state, mode_and_ctx, sizeof(mode_and_ctx)));

    return S2N_RESULT_OK;
}
