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

#include "tls/policy/s2n_policy_defaults.h"

#include "tls/s2n_security_policies.h"

/* clang-format off */
S2N_INLINE_SECURITY_POLICY_V1(
    default_policy_strict,
    S2N_TLS13,
    S2N_CIPHER_PREF_LIST(
        &s2n_tls13_aes_128_gcm_sha256,
        &s2n_tls13_aes_256_gcm_sha384,
    ),
    S2N_SIG_PREF_LIST(
        &s2n_mldsa44,
        &s2n_mldsa65,
        &s2n_mldsa87,
        &s2n_ecdsa_sha256,
        &s2n_ecdsa_sha384,
        &s2n_ecdsa_sha512,
        &s2n_rsa_pss_pss_sha256,
        &s2n_rsa_pss_pss_sha384,
        &s2n_rsa_pss_pss_sha512,
        &s2n_rsa_pss_rsae_sha256,
        &s2n_rsa_pss_rsae_sha384,
        &s2n_rsa_pss_rsae_sha512,
    ),
    S2N_CURVE_PREF_LIST(
        &s2n_ecc_curve_secp256r1,
        &s2n_ecc_curve_secp384r1,
        &s2n_ecc_curve_secp521r1,
    ),
    S2N_KEM_PREF_LIST(
        &s2n_secp256r1_mlkem_768,
        &s2n_x25519_mlkem_768,
        &s2n_secp384r1_mlkem_1024,
    )
);
/* clang-format on */

/* clang-format off */
S2N_INLINE_SECURITY_POLICY_V1(
    default_policy_compat,
    S2N_TLS12,
    S2N_CIPHER_PREF_LIST(
        &s2n_tls13_aes_128_gcm_sha256,
        &s2n_tls13_aes_256_gcm_sha384,
        &s2n_tls13_chacha20_poly1305_sha256,
        &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
        &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
        &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
        &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
        &s2n_ecdhe_ecdsa_with_chacha20_poly1305_sha256,
        &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256,
        &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
        &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
        &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
        &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    ),
    S2N_SIG_PREF_LIST(
        &s2n_mldsa44,
        &s2n_mldsa65,
        &s2n_mldsa87,
        &s2n_ecdsa_sha256,
        &s2n_ecdsa_sha384,
        &s2n_ecdsa_sha512,
        &s2n_rsa_pss_pss_sha256,
        &s2n_rsa_pss_pss_sha384,
        &s2n_rsa_pss_pss_sha512,
        &s2n_rsa_pss_rsae_sha256,
        &s2n_rsa_pss_rsae_sha384,
        &s2n_rsa_pss_rsae_sha512,
        &s2n_rsa_pkcs1_sha256,
        &s2n_rsa_pkcs1_sha384,
        &s2n_rsa_pkcs1_sha512,
    ),
    S2N_CURVE_PREF_LIST(
        &s2n_ecc_curve_secp256r1,
        &s2n_ecc_curve_x25519,
        &s2n_ecc_curve_secp384r1,
        &s2n_ecc_curve_secp521r1,
    ),
    S2N_KEM_PREF_LIST(
        &s2n_secp256r1_mlkem_768,
        &s2n_x25519_mlkem_768,
        &s2n_secp384r1_mlkem_1024,
    )
);
/* clang-format on */

const struct s2n_security_policy *default_policies[S2N_MAX_DEFAULT_POLICIES][S2N_MAX_POLICY_VERSIONS] = {
    [S2N_POLICY_STRICT] = {
            [S2N_STRICT_2025_08_20] = &default_policy_strict,
    },
    [S2N_POLICY_COMPATIBLE] = {
            [S2N_COMPAT_2025_08_20] = &default_policy_compat,
    },
};

const struct s2n_security_policy *s2n_security_policy_get(s2n_policy_name policy, uint64_t version)
{
    /* The uint64_t cast here is required for some older compilers to avoid a
     * "tautological-constant-out-of-range-compare" error. That error assumes
     * "policy" will be a valid s2n_default_policy, but that is not guaranteed by
     * the standard.
     */
    PTR_ENSURE((uint64_t) policy < S2N_MAX_DEFAULT_POLICIES, S2N_ERR_INVALID_SECURITY_POLICY);
    PTR_ENSURE(version < S2N_MAX_POLICY_VERSIONS, S2N_ERR_INVALID_SECURITY_POLICY);

    const struct s2n_security_policy *match = default_policies[policy][version];
    PTR_ENSURE(match, S2N_ERR_INVALID_SECURITY_POLICY);

    return match;
}
