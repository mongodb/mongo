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

#include "tls/s2n_signature_scheme.h"

#include "api/s2n.h"
#include "crypto/s2n_ecc_evp.h"
#include "crypto/s2n_hash.h"
#include "crypto/s2n_signature.h"
#include "tls/s2n_connection.h"
#include "utils/s2n_safety.h"

const struct s2n_signature_scheme s2n_null_sig_scheme = {
    .iana_value = 0,
    .iana_name = "null_sha0",
    .hash_alg = S2N_HASH_NONE,
    .sig_alg = S2N_SIGNATURE_ANONYMOUS,
    .libcrypto_nid = 0,
    .signature_curve = NULL,
    .maximum_protocol_version = 0,
};

/* RSA PKCS1 */
const struct s2n_signature_scheme s2n_rsa_pkcs1_md5_sha1 = {
    .iana_value = TLS_SIGNATURE_SCHEME_PRIVATE_INTERNAL_RSA_PKCS1_MD5_SHA1,
    .iana_name = "legacy_rsa_pkcs1_md5_sha1",
    .hash_alg = S2N_HASH_MD5_SHA1,
    .sig_alg = S2N_SIGNATURE_RSA,
    .libcrypto_nid = NID_md5_sha1,
    .signature_curve = NULL,               /* Elliptic Curve not needed for RSA */
    .maximum_protocol_version = S2N_TLS12, /* TLS1.3 does not support pkcs1 or sha1 */
};

const struct s2n_signature_scheme s2n_rsa_pkcs1_sha1 = {
    .iana_value = TLS_SIGNATURE_SCHEME_RSA_PKCS1_SHA1,
    .iana_name = "rsa_pkcs1_sha1",
    .hash_alg = S2N_HASH_SHA1,
    .sig_alg = S2N_SIGNATURE_RSA,
    .libcrypto_nid = NID_sha1WithRSAEncryption,
    .signature_curve = NULL,               /* Elliptic Curve not needed for RSA */
    .maximum_protocol_version = S2N_TLS12, /* TLS1.3 does not support pkcs1 or sha1 */
};

const struct s2n_signature_scheme s2n_rsa_pkcs1_sha224 = {
    .iana_value = TLS_SIGNATURE_SCHEME_RSA_PKCS1_SHA224,
    .iana_name = "legacy_rsa_pkcs1_sha224",
    .hash_alg = S2N_HASH_SHA224,
    .sig_alg = S2N_SIGNATURE_RSA,
    .libcrypto_nid = NID_sha224WithRSAEncryption,
    .signature_curve = NULL,               /* Elliptic Curve not needed for RSA */
    .maximum_protocol_version = S2N_TLS12, /* TLS1.3 does not support pkcs1 */
};

const struct s2n_signature_scheme s2n_rsa_pkcs1_sha256 = {
    .iana_value = TLS_SIGNATURE_SCHEME_RSA_PKCS1_SHA256,
    .iana_name = "rsa_pkcs1_sha256",
    .hash_alg = S2N_HASH_SHA256,
    .sig_alg = S2N_SIGNATURE_RSA,
    .libcrypto_nid = NID_sha256WithRSAEncryption,
    .signature_curve = NULL,               /* Elliptic Curve not needed for RSA */
    .maximum_protocol_version = S2N_TLS12, /* TLS1.3 does not support pkcs1 */
};

const struct s2n_signature_scheme s2n_rsa_pkcs1_sha384 = {
    .iana_value = TLS_SIGNATURE_SCHEME_RSA_PKCS1_SHA384,
    .iana_name = "rsa_pkcs1_sha384",
    .hash_alg = S2N_HASH_SHA384,
    .sig_alg = S2N_SIGNATURE_RSA,
    .libcrypto_nid = NID_sha384WithRSAEncryption,
    .signature_curve = NULL,               /* Elliptic Curve not needed for RSA */
    .maximum_protocol_version = S2N_TLS12, /* TLS1.3 does not support pkcs1 */
};

const struct s2n_signature_scheme s2n_rsa_pkcs1_sha512 = {
    .iana_value = TLS_SIGNATURE_SCHEME_RSA_PKCS1_SHA512,
    .iana_name = "rsa_pkcs1_sha512",
    .hash_alg = S2N_HASH_SHA512,
    .sig_alg = S2N_SIGNATURE_RSA,
    .libcrypto_nid = NID_sha512WithRSAEncryption,
    .signature_curve = NULL,               /* Elliptic Curve not needed for RSA */
    .maximum_protocol_version = S2N_TLS12, /* TLS1.3 does not support pkcs1 */
};

/* TLS 1.2 Compatible ECDSA Signature Schemes */
const struct s2n_signature_scheme s2n_ecdsa_sha1 = {
    .iana_value = TLS_SIGNATURE_SCHEME_ECDSA_SHA1,
    .iana_name = "ecdsa_sha1",
    .hash_alg = S2N_HASH_SHA1,
    .sig_alg = S2N_SIGNATURE_ECDSA,
    .libcrypto_nid = NID_ecdsa_with_SHA1,
    .signature_curve = NULL,               /* Decided by supported_groups Extension in TLS 1.2 and before */
    .maximum_protocol_version = S2N_TLS12, /* TLS1.3 does not support sha1 and requires a signature curve */
};

const struct s2n_signature_scheme s2n_ecdsa_sha224 = {
    .iana_value = TLS_SIGNATURE_SCHEME_ECDSA_SHA224,
    .iana_name = "legacy_ecdsa_sha224",
    .hash_alg = S2N_HASH_SHA224,
    .sig_alg = S2N_SIGNATURE_ECDSA,
    .libcrypto_nid = NID_ecdsa_with_SHA224,
    .signature_curve = NULL,               /* Decided by supported_groups Extension in TLS 1.2 and before */
    .maximum_protocol_version = S2N_TLS12, /* TLS1.3 requires a signature curve */
};

const struct s2n_signature_scheme s2n_ecdsa_sha256 = {
    .iana_value = TLS_SIGNATURE_SCHEME_ECDSA_SHA256,
    .iana_name = "ecdsa_sha256",
    .hash_alg = S2N_HASH_SHA256,
    .sig_alg = S2N_SIGNATURE_ECDSA,
    .libcrypto_nid = NID_ecdsa_with_SHA256,
    .signature_curve = &s2n_ecc_curve_secp256r1, /* Hardcoded for TLS 1.3 */
};

const struct s2n_signature_scheme s2n_ecdsa_sha384 = {
    .iana_value = TLS_SIGNATURE_SCHEME_ECDSA_SHA384,
    .iana_name = "ecdsa_sha384",
    .hash_alg = S2N_HASH_SHA384,
    .sig_alg = S2N_SIGNATURE_ECDSA,
    .libcrypto_nid = NID_ecdsa_with_SHA384,
    .signature_curve = &s2n_ecc_curve_secp384r1, /* Hardcoded for TLS 1.3 */
};

const struct s2n_signature_scheme s2n_ecdsa_sha512 = {
    .iana_value = TLS_SIGNATURE_SCHEME_ECDSA_SHA512,
    .iana_name = "ecdsa_sha512",
    .hash_alg = S2N_HASH_SHA512,
    .sig_alg = S2N_SIGNATURE_ECDSA,
    .libcrypto_nid = NID_ecdsa_with_SHA512,
    .signature_curve = &s2n_ecc_curve_secp521r1, /* Hardcoded for TLS 1.3 */
};

/**
 * RSA-PSS-RSAE
 */
const struct s2n_signature_scheme s2n_rsa_pss_rsae_sha256 = {
    .iana_value = TLS_SIGNATURE_SCHEME_RSA_PSS_RSAE_SHA256,
    .iana_name = "rsa_pss_rsae_sha256",
    .hash_alg = S2N_HASH_SHA256,
    .sig_alg = S2N_SIGNATURE_RSA_PSS_RSAE,
    .libcrypto_nid = NID_rsassaPss,
    .signature_curve = NULL, /* Elliptic Curve not needed for RSA */
};

const struct s2n_signature_scheme s2n_rsa_pss_rsae_sha384 = {
    .iana_value = TLS_SIGNATURE_SCHEME_RSA_PSS_RSAE_SHA384,
    .iana_name = "rsa_pss_rsae_sha384",
    .hash_alg = S2N_HASH_SHA384,
    .sig_alg = S2N_SIGNATURE_RSA_PSS_RSAE,
    .libcrypto_nid = NID_rsassaPss,
    .signature_curve = NULL, /* Elliptic Curve not needed for RSA */
};

const struct s2n_signature_scheme s2n_rsa_pss_rsae_sha512 = {
    .iana_value = TLS_SIGNATURE_SCHEME_RSA_PSS_RSAE_SHA512,
    .iana_name = "rsa_pss_rsae_sha512",
    .hash_alg = S2N_HASH_SHA512,
    .sig_alg = S2N_SIGNATURE_RSA_PSS_RSAE,
    .libcrypto_nid = NID_rsassaPss,
    .signature_curve = NULL, /* Elliptic Curve not needed for RSA */
};

/**
 * RSA-PSS-PSS
 */
const struct s2n_signature_scheme s2n_rsa_pss_pss_sha256 = {
    .iana_value = TLS_SIGNATURE_SCHEME_RSA_PSS_PSS_SHA256,
    .iana_name = "rsa_pss_pss_sha256",
    .hash_alg = S2N_HASH_SHA256,
    .sig_alg = S2N_SIGNATURE_RSA_PSS_PSS,
    .libcrypto_nid = NID_rsassaPss,
    .signature_curve = NULL, /* Elliptic Curve not needed for RSA */
    .minimum_protocol_version = S2N_TLS13,
};

const struct s2n_signature_scheme s2n_rsa_pss_pss_sha384 = {
    .iana_value = TLS_SIGNATURE_SCHEME_RSA_PSS_PSS_SHA384,
    .iana_name = "rsa_pss_pss_sha384",
    .hash_alg = S2N_HASH_SHA384,
    .sig_alg = S2N_SIGNATURE_RSA_PSS_PSS,
    .libcrypto_nid = NID_rsassaPss,
    .signature_curve = NULL, /* Elliptic Curve not needed for RSA */
    .minimum_protocol_version = S2N_TLS13,
};

const struct s2n_signature_scheme s2n_rsa_pss_pss_sha512 = {
    .iana_value = TLS_SIGNATURE_SCHEME_RSA_PSS_PSS_SHA512,
    .iana_name = "rsa_pss_pss_sha512",
    .hash_alg = S2N_HASH_SHA512,
    .sig_alg = S2N_SIGNATURE_RSA_PSS_PSS,
    .libcrypto_nid = NID_rsassaPss,
    .signature_curve = NULL, /* Elliptic Curve not needed for RSA */
    .minimum_protocol_version = S2N_TLS13,
};

/* Chosen based on AWS server recommendations as of 05/24.
 *
 * The recommendations do not include PKCS1, but we must include it anyway for
 * compatibility with older versions of our own defaults. Our old defaults only
 * supported PKCS1 for RSA, so would be unable to negotiate with a new default
 * that didn't include PKCS1.
 */
const struct s2n_signature_scheme* const s2n_sig_scheme_pref_list_20240501[] = {
    /* ECDSA */
    &s2n_ecdsa_sha256,
    &s2n_ecdsa_sha384,
    &s2n_ecdsa_sha512,

    /* RSA-PSS */
    &s2n_rsa_pss_pss_sha256,
    &s2n_rsa_pss_pss_sha384,
    &s2n_rsa_pss_pss_sha512,

    /* RSA */
    &s2n_rsa_pss_rsae_sha256,
    &s2n_rsa_pss_rsae_sha384,
    &s2n_rsa_pss_rsae_sha512,

    /* Legacy RSA with PKCS1 */
    &s2n_rsa_pkcs1_sha256,
    &s2n_rsa_pkcs1_sha384,
    &s2n_rsa_pkcs1_sha512,
};

const struct s2n_signature_preferences s2n_signature_preferences_20240501 = {
    .count = s2n_array_len(s2n_sig_scheme_pref_list_20240501),
    .signature_schemes = s2n_sig_scheme_pref_list_20240501,
};

/* All Supported SignatureSchemes. */
/* No MD5 to avoid SLOTH Vulnerability */
const struct s2n_signature_scheme* const s2n_sig_scheme_pref_list_20140601[] = {
    /* RSA PKCS1 */
    &s2n_rsa_pkcs1_sha256,
    &s2n_rsa_pkcs1_sha384,
    &s2n_rsa_pkcs1_sha512,
    &s2n_rsa_pkcs1_sha224,

    /* ECDSA */
    &s2n_ecdsa_sha256,
    &s2n_ecdsa_sha384,
    &s2n_ecdsa_sha512,
    &s2n_ecdsa_sha224,

    /* SHA-1 Legacy */
    &s2n_rsa_pkcs1_sha1,
    &s2n_ecdsa_sha1,
};

/* The original preference list, but with rsa_pss supported. */
const struct s2n_signature_scheme* const s2n_sig_scheme_pref_list_20200207[] = {
    /* RSA PSS */
    &s2n_rsa_pss_pss_sha256,
    &s2n_rsa_pss_pss_sha384,
    &s2n_rsa_pss_pss_sha512,
    &s2n_rsa_pss_rsae_sha256,
    &s2n_rsa_pss_rsae_sha384,
    &s2n_rsa_pss_rsae_sha512,

    /* RSA PKCS1 */
    &s2n_rsa_pkcs1_sha256,
    &s2n_rsa_pkcs1_sha384,
    &s2n_rsa_pkcs1_sha512,
    &s2n_rsa_pkcs1_sha224,

    /* ECDSA */
    &s2n_ecdsa_sha256,
    &s2n_ecdsa_sha384,
    &s2n_ecdsa_sha512,
    &s2n_ecdsa_sha224,

    /* SHA-1 Legacy */
    &s2n_rsa_pkcs1_sha1,
    &s2n_ecdsa_sha1,
};

/*
 * These signature schemes were chosen based on the following specification:
 * https://nvlpubs.nist.gov/nistpubs/SpecialPublications/NIST.SP.800-52r2.pdf
 */
const struct s2n_signature_scheme* const s2n_sig_scheme_pref_list_default_fips[] = {
    /* RSA PKCS1 - TLS1.2 */
    &s2n_rsa_pkcs1_sha256,
    &s2n_rsa_pkcs1_sha384,
    &s2n_rsa_pkcs1_sha512,

    /* ECDSA */
    &s2n_ecdsa_sha256,
    &s2n_ecdsa_sha384,
    &s2n_ecdsa_sha512,
    &s2n_ecdsa_sha224,
};

const struct s2n_signature_preferences s2n_signature_preferences_default_fips = {
    .count = s2n_array_len(s2n_sig_scheme_pref_list_default_fips),
    .signature_schemes = s2n_sig_scheme_pref_list_default_fips,
};

/*
 * FIPS compliant.
 * Supports TLS1.3.
 * Prefers PSS over PKCS1.
 */
const struct s2n_signature_scheme* const s2n_sig_scheme_pref_list_20230317[] = {
    /* RSA */
    &s2n_rsa_pss_rsae_sha256,
    &s2n_rsa_pss_rsae_sha384,
    &s2n_rsa_pss_rsae_sha512,
    &s2n_rsa_pkcs1_sha256,
    &s2n_rsa_pkcs1_sha384,
    &s2n_rsa_pkcs1_sha512,

    /* TLS1.2 with ECDSA */
    &s2n_ecdsa_sha256,
    &s2n_ecdsa_sha384,
    &s2n_ecdsa_sha512,

    /* TLS1.3 with RSA-PSS */
    &s2n_rsa_pss_pss_sha256,
    &s2n_rsa_pss_pss_sha384,
    &s2n_rsa_pss_pss_sha512,
};

const struct s2n_signature_preferences s2n_signature_preferences_20230317 = {
    .count = s2n_array_len(s2n_sig_scheme_pref_list_20230317),
    .signature_schemes = s2n_sig_scheme_pref_list_20230317,
};

const struct s2n_signature_scheme* const s2n_sig_scheme_pref_list_20201021[] = {
    /* RSA PSS */
    &s2n_rsa_pss_pss_sha256,
    &s2n_rsa_pss_pss_sha384,
    &s2n_rsa_pss_pss_sha512,
    &s2n_rsa_pss_rsae_sha256,
    &s2n_rsa_pss_rsae_sha384,
    &s2n_rsa_pss_rsae_sha512,

    /* RSA PKCS1 */
    &s2n_rsa_pkcs1_sha256,
    &s2n_rsa_pkcs1_sha384,
    &s2n_rsa_pkcs1_sha512,
    &s2n_rsa_pkcs1_sha224,

    /* ECDSA */
    &s2n_ecdsa_sha256,
    &s2n_ecdsa_sha384,
    &s2n_ecdsa_sha512,
    &s2n_ecdsa_sha224,

    /* SHA-1 Legacy */
    &s2n_rsa_pkcs1_sha1,
    &s2n_ecdsa_sha1,
};

const struct s2n_signature_preferences s2n_signature_preferences_20140601 = {
    .count = s2n_array_len(s2n_sig_scheme_pref_list_20140601),
    .signature_schemes = s2n_sig_scheme_pref_list_20140601,
};

const struct s2n_signature_preferences s2n_signature_preferences_20200207 = {
    .count = s2n_array_len(s2n_sig_scheme_pref_list_20200207),
    .signature_schemes = s2n_sig_scheme_pref_list_20200207,
};

const struct s2n_signature_preferences s2n_signature_preferences_20201021 = {
    .count = s2n_array_len(s2n_sig_scheme_pref_list_20201021),
    .signature_schemes = s2n_sig_scheme_pref_list_20201021,
};

const struct s2n_signature_preferences s2n_signature_preferences_null = {
    .count = 0,
    .signature_schemes = NULL,
};

/* TLS1.3 supported signature schemes, without SHA-1 legacy algorithms */
const struct s2n_signature_scheme* const s2n_sig_scheme_pref_list_20201110[] = {
    /* RSA PSS */
    &s2n_rsa_pss_pss_sha256,
    &s2n_rsa_pss_pss_sha384,
    &s2n_rsa_pss_pss_sha512,
    &s2n_rsa_pss_rsae_sha256,
    &s2n_rsa_pss_rsae_sha384,
    &s2n_rsa_pss_rsae_sha512,

    /* RSA PKCS1 */
    &s2n_rsa_pkcs1_sha256,
    &s2n_rsa_pkcs1_sha384,
    &s2n_rsa_pkcs1_sha512,
    &s2n_rsa_pkcs1_sha224,

    /* ECDSA */
    &s2n_ecdsa_sha256,
    &s2n_ecdsa_sha384,
    &s2n_ecdsa_sha512,
    &s2n_ecdsa_sha224,
};

const struct s2n_signature_preferences s2n_certificate_signature_preferences_20201110 = {
    .count = s2n_array_len(s2n_sig_scheme_pref_list_20201110),
    .signature_schemes = s2n_sig_scheme_pref_list_20201110,
};

/* Based on s2n_sig_scheme_pref_list_20140601 but with all hashes < SHA-384 removed */
const struct s2n_signature_scheme* const s2n_sig_scheme_pref_list_20210816[] = {
    /* RSA PKCS1 */
    &s2n_rsa_pkcs1_sha384,
    &s2n_rsa_pkcs1_sha512,

    /* ECDSA */
    &s2n_ecdsa_sha384,
    &s2n_ecdsa_sha512,
};

const struct s2n_signature_preferences s2n_signature_preferences_20210816 = {
    .count = s2n_array_len(s2n_sig_scheme_pref_list_20210816),
    .signature_schemes = s2n_sig_scheme_pref_list_20210816
};

const struct s2n_signature_scheme* const s2n_sig_scheme_pref_list_rfc9151[] = {
    /* ECDSA */
    &s2n_ecdsa_sha384,

    /* RSA PSS - TLS 1.3 */
    &s2n_rsa_pss_pss_sha384,

    /* RSA */
    &s2n_rsa_pss_rsae_sha384,

    &s2n_rsa_pkcs1_sha384,
};

const struct s2n_signature_scheme* const s2n_cert_sig_scheme_pref_list_rfc9151[] = {
    /* ECDSA */
    &s2n_ecdsa_sha384,

    /* RSA PSS
     * https://github.com/aws/s2n-tls/issues/3435
     *
     * The Openssl function used to parse signatures off certificates does not differentiate
     * between any rsa pss signature schemes. Therefore a security policy with a certificate
     * signatures preference list must include all rsa_pss signature schemes.
     *
     * Since only sha384 is allowed by rfc9151, this certificate signing policy does not
     * support rsa_pss.
     */

    /* RSA */
    &s2n_rsa_pkcs1_sha384,
};

const struct s2n_signature_preferences s2n_signature_preferences_rfc9151 = {
    .count = s2n_array_len(s2n_sig_scheme_pref_list_rfc9151),
    .signature_schemes = s2n_sig_scheme_pref_list_rfc9151
};

const struct s2n_signature_preferences s2n_certificate_signature_preferences_rfc9151 = {
    .count = s2n_array_len(s2n_cert_sig_scheme_pref_list_rfc9151),
    .signature_schemes = s2n_cert_sig_scheme_pref_list_rfc9151
};

/*
 * Legacy compatibility policy based on s2n_sig_scheme_pref_list_20201021 with ECDSA prioritized.
 * This list also removes ECDSA+SHA224, which is not known to be relied on by any legitimate
 * use cases.
 */
const struct s2n_signature_scheme* const s2n_sig_scheme_pref_list_20240521[] = {
    /* ECDSA */
    &s2n_ecdsa_sha256,
    &s2n_ecdsa_sha384,
    &s2n_ecdsa_sha512,

    /* RSA PSS */
    &s2n_rsa_pss_pss_sha256,
    &s2n_rsa_pss_pss_sha384,
    &s2n_rsa_pss_pss_sha512,
    &s2n_rsa_pss_rsae_sha256,
    &s2n_rsa_pss_rsae_sha384,
    &s2n_rsa_pss_rsae_sha512,

    /* RSA PKCS1 */
    &s2n_rsa_pkcs1_sha256,
    &s2n_rsa_pkcs1_sha384,
    &s2n_rsa_pkcs1_sha512,
    &s2n_rsa_pkcs1_sha224,

    /* SHA-1 Legacy */
    &s2n_ecdsa_sha1,
    &s2n_rsa_pkcs1_sha1,
};

const struct s2n_signature_preferences s2n_signature_preferences_20240521 = {
    .count = s2n_array_len(s2n_sig_scheme_pref_list_20240521),
    .signature_schemes = s2n_sig_scheme_pref_list_20240521
};

const struct s2n_signature_scheme* const s2n_sig_scheme_pref_list_test_all_fips[] = {
    /* RSA PSS */
    &s2n_rsa_pss_pss_sha256,
    &s2n_rsa_pss_pss_sha384,
    &s2n_rsa_pss_pss_sha512,
    &s2n_rsa_pss_rsae_sha256,
    &s2n_rsa_pss_rsae_sha384,
    &s2n_rsa_pss_rsae_sha512,

    /* RSA PKCS1 */
    &s2n_rsa_pkcs1_sha256,
    &s2n_rsa_pkcs1_sha384,
    &s2n_rsa_pkcs1_sha512,
    &s2n_rsa_pkcs1_sha224,

    /* ECDSA */
    &s2n_ecdsa_sha256,
    &s2n_ecdsa_sha384,
    &s2n_ecdsa_sha512,
    &s2n_ecdsa_sha224,
};

const struct s2n_signature_preferences s2n_signature_preferences_test_all_fips = {
    .count = s2n_array_len(s2n_sig_scheme_pref_list_test_all_fips),
    .signature_schemes = s2n_sig_scheme_pref_list_test_all_fips,
};
