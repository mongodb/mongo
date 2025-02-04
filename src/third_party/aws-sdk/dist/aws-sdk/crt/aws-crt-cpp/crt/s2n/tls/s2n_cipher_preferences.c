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

#include "tls/s2n_cipher_preferences.h"

#include <stdint.h>
#include <strings.h>

#include "api/s2n.h"
#include "error/s2n_errno.h"
#include "tls/s2n_config.h"
#include "tls/s2n_kem.h"
#include "tls/s2n_kex.h"
#include "utils/s2n_safety.h"

/* clang-format off */
/* TLS 1.3 cipher suites, in order of preference.
 * Can be added to other ciphers suite lists to enable
 * TLS1.3 compatibility. */
#define S2N_TLS13_CIPHER_SUITES_20190801 \
    &s2n_tls13_aes_256_gcm_sha384,       \
    &s2n_tls13_aes_128_gcm_sha256,       \
    &s2n_tls13_chacha20_poly1305_sha256

#define S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716 \
    &s2n_tls13_aes_128_gcm_sha256,       \
    &s2n_tls13_aes_256_gcm_sha384,       \
    &s2n_tls13_chacha20_poly1305_sha256

/* s2n's list of cipher suites, in order of preferences, as of 2019-08-01 */
struct s2n_cipher_suite *cipher_suites_20190801[] = {
    S2N_TLS13_CIPHER_SUITES_20190801,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha
};

const struct s2n_cipher_preferences cipher_preferences_20190801 = {
    .count = s2n_array_len(cipher_suites_20190801),
    .suites = cipher_suites_20190801,
    .allow_chacha20_boosting = false,
};

/* Same as 20190801, but with ECDSA for TLS 1.2 added */
struct s2n_cipher_suite *cipher_suites_20210831[] = {
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha
};

const struct s2n_cipher_preferences cipher_preferences_20210831 = {
    .count = s2n_array_len(cipher_suites_20210831),
    .suites = cipher_suites_20210831,
    .allow_chacha20_boosting = false,
};

/*
 * These cipher suites were chosen based on the following specification:
 * https://nvlpubs.nist.gov/nistpubs/SpecialPublications/NIST.SP.800-52r2.pdf
 */
struct s2n_cipher_suite *cipher_suites_default_fips[] = {
    /* tls1.2 with ECDSA */
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,

    /* tls1.2 with RSA */
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
    &s2n_dhe_rsa_with_aes_128_gcm_sha256,
    &s2n_dhe_rsa_with_aes_256_gcm_sha384,
};

const struct s2n_cipher_preferences cipher_preferences_default_fips = {
    .count = s2n_array_len(cipher_suites_default_fips),
    .suites = cipher_suites_default_fips,
    .allow_chacha20_boosting = false,
};

/* s2n's list of cipher suites, in order of preference, as of 2014-06-01 */
struct s2n_cipher_suite *cipher_suites_20140601[] = {
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
    &s2n_dhe_rsa_with_3des_ede_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_rsa_with_rc4_128_sha,
    &s2n_rsa_with_rc4_128_md5
};

const struct s2n_cipher_preferences cipher_preferences_20140601 = {
    .count = s2n_array_len(cipher_suites_20140601),
    .suites = cipher_suites_20140601,
    .allow_chacha20_boosting = false,
};

/* Disable SSLv3 due to POODLE */
const struct s2n_cipher_preferences cipher_preferences_20141001 = {
    .count = s2n_array_len(cipher_suites_20140601),
    .suites = cipher_suites_20140601,
    .allow_chacha20_boosting = false,
};

/* Disable RC4 */
struct s2n_cipher_suite *cipher_suites_20150202[] = {
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
    &s2n_dhe_rsa_with_3des_ede_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_3des_ede_cbc_sha
};

const struct s2n_cipher_preferences cipher_preferences_20150202 = {
    .count = s2n_array_len(cipher_suites_20150202),
    .suites = cipher_suites_20150202,
    .allow_chacha20_boosting = false,
};

/* Support AES-GCM modes */
struct s2n_cipher_suite *cipher_suites_20150214[] = {
    &s2n_dhe_rsa_with_aes_128_gcm_sha256,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
    &s2n_dhe_rsa_with_3des_ede_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_3des_ede_cbc_sha
};

const struct s2n_cipher_preferences cipher_preferences_20150214 = {
    .count = s2n_array_len(cipher_suites_20150214),
    .suites = cipher_suites_20150214,
    .allow_chacha20_boosting = false,
};

/* Make a CBC cipher #1 to avoid negotiating GCM with buggy Java clients */
struct s2n_cipher_suite *cipher_suites_20160411[] = {
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_3des_ede_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_20160411 = {
    .count = s2n_array_len(cipher_suites_20160411),
    .suites = cipher_suites_20160411,
    .allow_chacha20_boosting = false,
};

/* Use ECDHE instead of plain DHE. Prioritize ECDHE in favour of non ECDHE; GCM in favour of CBC; AES128 in favour of AES256. */
struct s2n_cipher_suite *cipher_suites_20150306[] = {
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_3des_ede_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_3des_ede_cbc_sha
};

const struct s2n_cipher_preferences cipher_preferences_20150306 = {
    .count = s2n_array_len(cipher_suites_20150306),
    .suites = cipher_suites_20150306,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_20160804[] = {
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_3des_ede_cbc_sha
};

const struct s2n_cipher_preferences cipher_preferences_20160804 = {
    .count = s2n_array_len(cipher_suites_20160804),
    .suites = cipher_suites_20160804,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_20160824[] = {
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha
};

const struct s2n_cipher_preferences cipher_preferences_20160824 = {
    .count = s2n_array_len(cipher_suites_20160824),
    .suites = cipher_suites_20160824,
    .allow_chacha20_boosting = false,
};

/* Add ChaCha20 suite */
struct s2n_cipher_suite *cipher_suites_20170210[] = {
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha
};

const struct s2n_cipher_preferences cipher_preferences_20170210 = {
    .count = s2n_array_len(cipher_suites_20170210),
    .suites = cipher_suites_20170210,
    .allow_chacha20_boosting = false,
};

/*
 * TLS1.3 support.
 * FIPS compliant.
 * No DHE (would require extra setup with s2n_config_add_dhparams)
 */
struct s2n_cipher_suite *cipher_suites_20230317[] = {
    /* TLS1.2 with ECDSA */
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,

    /* TLS1.2 with RSA */
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,

    /* TLS1.3 */
    &s2n_tls13_aes_128_gcm_sha256,
    &s2n_tls13_aes_256_gcm_sha384,
};

const struct s2n_cipher_preferences cipher_preferences_20230317 = {
    .count = s2n_array_len(cipher_suites_20230317),
    .suites = cipher_suites_20230317,
    .allow_chacha20_boosting = false,
};

/*
 * No TLS1.3 support.
 * FIPS compliant.
 * No DHE (would require extra setup with s2n_config_add_dhparams)
 */
struct s2n_cipher_suite *cipher_suites_20240331[] = {
    /* TLS1.2 with ECDSA */
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,

    /* TLS1.2 with RSA */
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
};

const struct s2n_cipher_preferences cipher_preferences_20240331 = {
    .count = s2n_array_len(cipher_suites_20240331),
    .suites = cipher_suites_20240331,
    .allow_chacha20_boosting = false,
};

/* Same as 20160411, but with ChaCha20 added as 1st in Preference List */
struct s2n_cipher_suite *cipher_suites_20190122[] = {
    &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_3des_ede_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_20190122 = {
    .count = s2n_array_len(cipher_suites_20190122),
    .suites = cipher_suites_20190122,
    .allow_chacha20_boosting = false,
};

/* Same as 20160804, but with ChaCha20 added as 2nd in Preference List */
struct s2n_cipher_suite *cipher_suites_20190121[] = {
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_3des_ede_cbc_sha
};

const struct s2n_cipher_preferences cipher_preferences_20190121 = {
    .count = s2n_array_len(cipher_suites_20190121),
    .suites = cipher_suites_20190121,
    .allow_chacha20_boosting = false,
};

/* Same as 20160411, but with ChaCha20 in 3rd Place after CBC and GCM */
struct s2n_cipher_suite *cipher_suites_20190120[] = {
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_3des_ede_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_20190120 = {
    .count = s2n_array_len(cipher_suites_20190120),
    .suites = cipher_suites_20190120,
    .allow_chacha20_boosting = false,
};

/* Preferences optimized for interop, includes ECDSA priortitized. DHE and 3DES are added(at the lowest preference). */
struct s2n_cipher_suite *cipher_suites_20190214[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_gcm_sha256,
    &s2n_dhe_rsa_with_aes_256_gcm_sha384,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
};

const struct s2n_cipher_preferences cipher_preferences_20190214 = {
    .count = s2n_array_len(cipher_suites_20190214),
    .suites = cipher_suites_20190214,
    .allow_chacha20_boosting = false,
};

/* 20190214 with aes-gcm prioritized above aes-cbc */
struct s2n_cipher_suite *cipher_suites_20190214_gcm[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_gcm_sha256,
    &s2n_dhe_rsa_with_aes_256_gcm_sha384,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
};

const struct s2n_cipher_preferences cipher_preferences_20190214_gcm = {
    .count = s2n_array_len(cipher_suites_20190214_gcm),
    .suites = cipher_suites_20190214_gcm,
    .allow_chacha20_boosting = false,
};

/* Same as cipher_suites_20190214, but with TLS 1.3 Ciphers */
struct s2n_cipher_suite *cipher_suites_20210825[] = {
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_gcm_sha256,
    &s2n_dhe_rsa_with_aes_256_gcm_sha384,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
};

const struct s2n_cipher_preferences cipher_preferences_20210825 = {
    .count = s2n_array_len(cipher_suites_20210825),
    .suites = cipher_suites_20210825,
    .allow_chacha20_boosting = false,
};

/* Same as cipher_suites_20190214_gcm, but with TLS 1.3 Ciphers */
struct s2n_cipher_suite *cipher_suites_20210825_gcm[] = {
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_gcm_sha256,
    &s2n_dhe_rsa_with_aes_256_gcm_sha384,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
};

const struct s2n_cipher_preferences cipher_preferences_20210825_gcm = {
    .count = s2n_array_len(cipher_suites_20210825_gcm),
    .suites = cipher_suites_20210825_gcm,
    .allow_chacha20_boosting = false,
};

/* Same as cipher_suites_20210825, but with 3DES removed */
struct s2n_cipher_suite *cipher_suites_20241008[] = {
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_gcm_sha256,
    &s2n_dhe_rsa_with_aes_256_gcm_sha384,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
};

const struct s2n_cipher_preferences cipher_preferences_20241008 = {
    .count = s2n_array_len(cipher_suites_20241008),
    .suites = cipher_suites_20241008,
    .allow_chacha20_boosting = false,
};

/* Same as cipher_suites_20210825_gcm and cipher_suites_pq_tls_1_0_2021_05_26, but with 3DES and Kyber (for TLS 1.2) removed */
struct s2n_cipher_suite *cipher_suites_20241008_gcm[] = {
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_dhe_rsa_with_aes_128_gcm_sha256,
    &s2n_dhe_rsa_with_aes_256_gcm_sha384,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
};

const struct s2n_cipher_preferences cipher_preferences_20241008_gcm = {
    .count = s2n_array_len(cipher_suites_20241008_gcm),
    .suites = cipher_suites_20241008_gcm,
    .allow_chacha20_boosting = false,
};

/* Same as cipher_preferences_pq_tls_1_0_2021_05_24, but with 3DES and Kyber (for TLS 1.2) removed */
struct s2n_cipher_suite *cipher_suites_20241009[] = {
    S2N_TLS13_CIPHER_SUITES_20190801,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_20241009 = {
    .count = s2n_array_len(cipher_suites_20241009),
    .suites = cipher_suites_20241009,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_null[] = {
    &s2n_null_cipher_suite
};

const struct s2n_cipher_preferences cipher_preferences_null = {
    .count = s2n_array_len(cipher_suites_null),
    .suites = cipher_suites_null,
    .allow_chacha20_boosting = false,
};

/* Preferences optimized for interop. DHE and 3DES are added(at the lowest preference). */
struct s2n_cipher_suite *cipher_suites_20170328[] = {
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_gcm_sha256,
    &s2n_dhe_rsa_with_aes_256_gcm_sha384,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
};

const struct s2n_cipher_preferences cipher_preferences_20170328 = {
    .count = s2n_array_len(cipher_suites_20170328),
    .suites = cipher_suites_20170328,
    .allow_chacha20_boosting = false,
};

/* Equivalent to cipher_suites_20170328 with aes-gcm prioritized above aes-cbc */
struct s2n_cipher_suite *cipher_suites_20170328_gcm[] = {
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_gcm_sha256,
    &s2n_dhe_rsa_with_aes_256_gcm_sha384,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
};

const struct s2n_cipher_preferences cipher_preferences_20170328_gcm = {
    .count = s2n_array_len(cipher_suites_20170328_gcm),
    .suites = cipher_suites_20170328_gcm,
    .allow_chacha20_boosting = false,
};

/* Preferences optimized for FIPS compatibility. */
struct s2n_cipher_suite *cipher_suites_20170405[] = {
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_3des_ede_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_20170405 = {
    .count = s2n_array_len(cipher_suites_20170405),
    .suites = cipher_suites_20170405,
    .allow_chacha20_boosting = false,
};

/* Preferences optimized for FIPS compatibility with GCM prioritized */
struct s2n_cipher_suite *cipher_suites_20170405_gcm[] = {
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_3des_ede_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_20170405_gcm = {
    .count = s2n_array_len(cipher_suites_20170405_gcm),
    .suites = cipher_suites_20170405_gcm,
    .allow_chacha20_boosting = false,
};

/* Equivalent to cipher_suite_20160411 with 3DES removed.
 * Make a CBC cipher #1 to avoid negotiating GCM with buggy Java clients. */
struct s2n_cipher_suite *cipher_suites_20170718[] = {
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
};

const struct s2n_cipher_preferences cipher_preferences_20170718 = {
    .count = s2n_array_len(cipher_suites_20170718),
    .suites = cipher_suites_20170718,
    .allow_chacha20_boosting = false,
};

/* Equivalent to cipher_suites_20170718 with aes-gcm prioritized above aes-cbc */
struct s2n_cipher_suite *cipher_suites_20170718_gcm[] = {
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
};

const struct s2n_cipher_preferences cipher_preferences_20170718_gcm = {
    .count = s2n_array_len(cipher_suites_20170718_gcm),
    .suites = cipher_suites_20170718_gcm,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_elb_security_policy_2015_04[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_3des_ede_cbc_sha,
};

const struct s2n_cipher_preferences elb_security_policy_2015_04 = {
    .count = s2n_array_len(cipher_suites_elb_security_policy_2015_04),
    .suites = cipher_suites_elb_security_policy_2015_04,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_elb_security_policy_2016_08[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
};

const struct s2n_cipher_preferences elb_security_policy_2016_08 = {
    .count = s2n_array_len(cipher_suites_elb_security_policy_2016_08),
    .suites = cipher_suites_elb_security_policy_2016_08,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_elb_security_policy_tls_1_2_2017_01[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_256_cbc_sha256,
};

const struct s2n_cipher_preferences elb_security_policy_tls_1_2_2017_01 = {
    .count = s2n_array_len(cipher_suites_elb_security_policy_tls_1_2_2017_01),
    .suites = cipher_suites_elb_security_policy_tls_1_2_2017_01,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_elb_security_policy_tls_1_1_2017_01[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
};

const struct s2n_cipher_preferences elb_security_policy_tls_1_1_2017_01 = {
    .count = s2n_array_len(cipher_suites_elb_security_policy_tls_1_1_2017_01),
    .suites = cipher_suites_elb_security_policy_tls_1_1_2017_01,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_elb_security_policy_tls_1_2_ext_2018_06[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
};

const struct s2n_cipher_preferences elb_security_policy_tls_1_2_ext_2018_06 = {
    .count = s2n_array_len(cipher_suites_elb_security_policy_tls_1_2_ext_2018_06),
    .suites = cipher_suites_elb_security_policy_tls_1_2_ext_2018_06,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_elb_security_policy_fs_2018_06[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
};

const struct s2n_cipher_preferences elb_security_policy_fs_2018_06 = {
    .count = s2n_array_len(cipher_suites_elb_security_policy_fs_2018_06),
    .suites = cipher_suites_elb_security_policy_fs_2018_06,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_elb_security_policy_fs_1_2_2019_08[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
};

const struct s2n_cipher_preferences elb_security_policy_fs_1_2_2019_08 = {
    .count = s2n_array_len(cipher_suites_elb_security_policy_fs_1_2_2019_08),
    .suites = cipher_suites_elb_security_policy_fs_1_2_2019_08,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_elb_security_policy_fs_1_1_2019_08[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
};

const struct s2n_cipher_preferences elb_security_policy_fs_1_1_2019_08 = {
    .count = s2n_array_len(cipher_suites_elb_security_policy_fs_1_1_2019_08),
    .suites = cipher_suites_elb_security_policy_fs_1_1_2019_08,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_elb_security_policy_fs_1_2_Res_2019_08[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
};

const struct s2n_cipher_preferences elb_security_policy_fs_1_2_Res_2019_08 = {
    .count = s2n_array_len(cipher_suites_elb_security_policy_fs_1_2_Res_2019_08),
    .suites = cipher_suites_elb_security_policy_fs_1_2_Res_2019_08,
    .allow_chacha20_boosting = false,
};

/*
 * S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716 on top of cipher_suites_elb_security_policy_tls_1_2_ext_2018_06
*/
struct s2n_cipher_suite *cipher_suites_elb_security_policy_tls13_1_2_Ext2_2021_06[] = {
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
};

const struct s2n_cipher_preferences elb_security_policy_tls13_1_2_Ext2_2021_06 = {
    .count = s2n_array_len(cipher_suites_elb_security_policy_tls13_1_2_Ext2_2021_06),
    .suites = cipher_suites_elb_security_policy_tls13_1_2_Ext2_2021_06,
};

struct s2n_cipher_suite *cipher_suites_cloudfront_upstream[] = {
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_rsa_with_rc4_128_md5
};

const struct s2n_cipher_preferences cipher_preferences_cloudfront_upstream = {
    .count = s2n_array_len(cipher_suites_cloudfront_upstream),
    .suites = cipher_suites_cloudfront_upstream,
    .allow_chacha20_boosting = false,
};

/* CloudFront viewer facing (with TLS 1.3) */
struct s2n_cipher_suite *cipher_suites_cloudfront_ssl_v_3[] = {
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_rsa_with_rc4_128_md5
};

const struct s2n_cipher_preferences cipher_preferences_cloudfront_ssl_v_3 = {
    .count = s2n_array_len(cipher_suites_cloudfront_ssl_v_3),
    .suites = cipher_suites_cloudfront_ssl_v_3,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_cloudfront_tls_1_0_2014[] = {
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_3des_ede_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_0_2014 = {
    .count = s2n_array_len(cipher_suites_cloudfront_tls_1_0_2014),
    .suites = cipher_suites_cloudfront_tls_1_0_2014,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_cloudfront_tls_1_0_2016[] = {
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha
};

const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_0_2016 = {
    .count = s2n_array_len(cipher_suites_cloudfront_tls_1_0_2016),
    .suites = cipher_suites_cloudfront_tls_1_0_2016,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_cloudfront_tls_1_1_2016[] = {
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha
};

const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_1_2016 = {
    .count = s2n_array_len(cipher_suites_cloudfront_tls_1_1_2016),
    .suites = cipher_suites_cloudfront_tls_1_1_2016,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_cloudfront_tls_1_2_2017[] = {
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256
};

const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_2_2017 = {
    .count = s2n_array_len(cipher_suites_cloudfront_tls_1_2_2017),
    .suites = cipher_suites_cloudfront_tls_1_2_2017,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_cloudfront_tls_1_2_2018[] = {
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256
};

const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_2_2018 = {
    .count = s2n_array_len(cipher_suites_cloudfront_tls_1_2_2018),
    .suites = cipher_suites_cloudfront_tls_1_2_2018,
    .allow_chacha20_boosting = false,
};

/* CloudFront viewer facing legacy TLS 1.2 policies */
struct s2n_cipher_suite *cipher_suites_cloudfront_ssl_v_3_legacy[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_rsa_with_rc4_128_md5
};

const struct s2n_cipher_preferences cipher_preferences_cloudfront_ssl_v_3_legacy = {
    .count = s2n_array_len(cipher_suites_cloudfront_ssl_v_3_legacy),
    .suites = cipher_suites_cloudfront_ssl_v_3_legacy,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_cloudfront_tls_1_0_2014_legacy[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_3des_ede_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_0_2014_legacy = {
    .count = s2n_array_len(cipher_suites_cloudfront_tls_1_0_2014_legacy),
    .suites = cipher_suites_cloudfront_tls_1_0_2014_legacy,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_cloudfront_tls_1_0_2016_legacy[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha
};

const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_0_2016_legacy = {
    .count = s2n_array_len(cipher_suites_cloudfront_tls_1_0_2016_legacy),
    .suites = cipher_suites_cloudfront_tls_1_0_2016_legacy,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_cloudfront_tls_1_1_2016_legacy[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha
};

const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_1_2016_legacy = {
    .count = s2n_array_len(cipher_suites_cloudfront_tls_1_1_2016_legacy),
    .suites = cipher_suites_cloudfront_tls_1_1_2016_legacy,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_cloudfront_tls_1_2_2018_legacy[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256
};

const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_2_2018_legacy = {
    .count = s2n_array_len(cipher_suites_cloudfront_tls_1_2_2018_legacy),
    .suites = cipher_suites_cloudfront_tls_1_2_2018_legacy,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_cloudfront_tls_1_2_2019_legacy[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256
};

const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_2_2019_legacy = {
    .count = s2n_array_len(cipher_suites_cloudfront_tls_1_2_2019_legacy),
    .suites = cipher_suites_cloudfront_tls_1_2_2019_legacy,
    .allow_chacha20_boosting = false,
};

/* CloudFront upstream */
struct s2n_cipher_suite *cipher_suites_cloudfront_upstream_tls10[] = {
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_rsa_with_rc4_128_md5
};

const struct s2n_cipher_preferences cipher_preferences_cloudfront_upstream_tls10 = {
    .count = s2n_array_len(cipher_suites_cloudfront_upstream_tls10),
    .suites = cipher_suites_cloudfront_upstream_tls10,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_cloudfront_upstream_tls11[] = {
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_rsa_with_rc4_128_md5
};

const struct s2n_cipher_preferences cipher_preferences_cloudfront_upstream_tls11 = {
    .count = s2n_array_len(cipher_suites_cloudfront_upstream_tls11),
    .suites = cipher_suites_cloudfront_upstream_tls11,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_cloudfront_upstream_tls12[] = {
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_rsa_with_rc4_128_md5
};

const struct s2n_cipher_preferences cipher_preferences_cloudfront_upstream_tls12 = {
    .count = s2n_array_len(cipher_suites_cloudfront_upstream_tls12),
    .suites = cipher_suites_cloudfront_upstream_tls12,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_cloudfront_tls_1_2_2019[] = {
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256
};

const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_2_2019 = {
    .count = s2n_array_len(cipher_suites_cloudfront_tls_1_2_2019),
    .suites = cipher_suites_cloudfront_tls_1_2_2019,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_cloudfront_tls_1_2_2021[] = {
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_chacha20_poly1305_sha256,
    &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256
};

const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_2_2021 = {
    .count = s2n_array_len(cipher_suites_cloudfront_tls_1_2_2021),
    .suites = cipher_suites_cloudfront_tls_1_2_2021,
    .allow_chacha20_boosting = false,
};

/* Duplicate of cipher_preferences_cloudfront_tls_1_2_2021 but with allow_chacha20_boosting enabled */
const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_2_2021_chacha20_boosted = {
    .count = s2n_array_len(cipher_suites_cloudfront_tls_1_2_2021),
    .suites = cipher_suites_cloudfront_tls_1_2_2021,
    .allow_chacha20_boosting = true,
};

/* Based on cipher_preferences_cloudfront_tls_1_0_2016, but with ordering changed and AES256-SHA256, DES-CBC3-SHA, and
 * RC4-MD5 added for compatibility. */
struct s2n_cipher_suite *cipher_suites_aws_crt_sdk_ssl_v3[] = {
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
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
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_rsa_with_rc4_128_md5
};

const struct s2n_cipher_preferences cipher_preferences_aws_crt_sdk_ssl_v3 = {
    .count = s2n_array_len(cipher_suites_aws_crt_sdk_ssl_v3),
    .suites = cipher_suites_aws_crt_sdk_ssl_v3,
    .allow_chacha20_boosting = false,
};

/* Based on cipher_preferences_cloudfront_tls_1_0_2016, but with ordering changed and AES256-SHA256 added for
 * compatibility. */
struct s2n_cipher_suite *cipher_suites_aws_crt_sdk_default[] = {
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
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
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_aws_crt_sdk_default = {
    .count = s2n_array_len(cipher_suites_aws_crt_sdk_default),
    .suites = cipher_suites_aws_crt_sdk_default,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_aws_crt_sdk_tls_13[] = {
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716
};

const struct s2n_cipher_preferences cipher_preferences_aws_crt_sdk_tls_13 = {
    .count = s2n_array_len(cipher_suites_aws_crt_sdk_tls_13),
    .suites = cipher_suites_aws_crt_sdk_tls_13,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_kms_tls_1_0_2018_10[] = {
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_3des_ede_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_kms_tls_1_0_2018_10 = {
    .count = s2n_array_len(cipher_suites_kms_tls_1_0_2018_10),
    .suites = cipher_suites_kms_tls_1_0_2018_10,
    .allow_chacha20_boosting = false,
};


struct s2n_cipher_suite *cipher_suites_kms_tls_1_0_2021_08[] = {
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_3des_ede_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_kms_tls_1_0_2021_08 = {
    .count = s2n_array_len(cipher_suites_kms_tls_1_0_2021_08),
    .suites = cipher_suites_kms_tls_1_0_2021_08,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_kms_pq_tls_1_0_2019_06[] = {
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_3des_ede_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
};

/* Includes only round 1 PQ KEM params */
const struct s2n_cipher_preferences cipher_preferences_kms_pq_tls_1_0_2019_06 = {
    .count = s2n_array_len(cipher_suites_kms_pq_tls_1_0_2019_06),
    .suites = cipher_suites_kms_pq_tls_1_0_2019_06,
    .allow_chacha20_boosting = false,
};

/* Includes round 1 and round 2 PQ KEM params. The cipher suite list is the same
 * as in cipher_preferences_kms_pq_tls_1_0_2019_06.*/
const struct s2n_cipher_preferences cipher_preferences_kms_pq_tls_1_0_2020_02 = {
    .count = s2n_array_len(cipher_suites_kms_pq_tls_1_0_2019_06),
    .suites = cipher_suites_kms_pq_tls_1_0_2019_06,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_pq_sike_test_tls_1_0_2019_11[] = {
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_3des_ede_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
};

/* Previously included only SIKE round 1 (for integration tests) */
const struct s2n_cipher_preferences cipher_preferences_pq_sike_test_tls_1_0_2019_11 = {
    .count = s2n_array_len(cipher_suites_pq_sike_test_tls_1_0_2019_11),
    .suites = cipher_suites_pq_sike_test_tls_1_0_2019_11,
    .allow_chacha20_boosting = false,
};

/* Previously included SIKE round 1 and round 2 (for integration tests). The cipher suite list
 * is the same as in cipher_preferences_pq_sike_test_tls_1_0_2019_11. */
const struct s2n_cipher_preferences cipher_preferences_pq_sike_test_tls_1_0_2020_02 = {
    .count = s2n_array_len(cipher_suites_pq_sike_test_tls_1_0_2019_11),
    .suites = cipher_suites_pq_sike_test_tls_1_0_2019_11,
    .allow_chacha20_boosting = false,
};

/* Includes Kyber PQ algorithm */
struct s2n_cipher_suite *cipher_suites_kms_pq_tls_1_0_2020_07[] = {
    &s2n_ecdhe_kyber_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_3des_ede_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_kms_pq_tls_1_0_2020_07 = {
    .count = s2n_array_len(cipher_suites_kms_pq_tls_1_0_2020_07),
    .suites = cipher_suites_kms_pq_tls_1_0_2020_07,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_pq_tls_1_0_2020_12[] = {
        S2N_TLS13_CIPHER_SUITES_20190801,
        &s2n_ecdhe_kyber_rsa_with_aes_256_gcm_sha384,
        &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
        &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
        &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
        &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
        &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
        &s2n_ecdhe_rsa_with_3des_ede_cbc_sha,
        &s2n_dhe_rsa_with_aes_256_cbc_sha256,
        &s2n_dhe_rsa_with_aes_128_cbc_sha256,
        &s2n_dhe_rsa_with_aes_256_cbc_sha,
        &s2n_dhe_rsa_with_aes_128_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_pq_tls_1_0_2020_12 = {
        .count = s2n_array_len(cipher_suites_pq_tls_1_0_2020_12),
        .suites = cipher_suites_pq_tls_1_0_2020_12,
        .allow_chacha20_boosting = false,
};

/* Same as ELBSecurityPolicy-TLS-1-1-2017-01, but with PQ Ciphers appended to top of preference list */
struct s2n_cipher_suite *cipher_suites_pq_tls_1_1_2021_05_17[] = {
    &s2n_ecdhe_kyber_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_pq_tls_1_1_2021_05_17 = {
    .count = s2n_array_len(cipher_suites_pq_tls_1_1_2021_05_17),
    .suites = cipher_suites_pq_tls_1_1_2021_05_17,
    .allow_chacha20_boosting = false,
};

/* Same as cipher_preferences_20190214, but with PQ Ciphers appended to top of preference list */
struct s2n_cipher_suite *cipher_suites_pq_tls_1_0_2021_05_18[] = {
    &s2n_ecdhe_kyber_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_gcm_sha256,
    &s2n_dhe_rsa_with_aes_256_gcm_sha384,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
};

const struct s2n_cipher_preferences cipher_preferences_pq_tls_1_0_2021_05_18 = {
    .count = s2n_array_len(cipher_suites_pq_tls_1_0_2021_05_18),
    .suites = cipher_suites_pq_tls_1_0_2021_05_18,
    .allow_chacha20_boosting = false,
};

/* Same as ELBSecurityPolicy-2016-08, but with PQ Ciphers appended to top of preference list */
struct s2n_cipher_suite *cipher_suites_pq_tls_1_0_2021_05_19[] = {
    &s2n_ecdhe_kyber_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_pq_tls_1_0_2021_05_19 = {
    .count = s2n_array_len(cipher_suites_pq_tls_1_0_2021_05_19),
    .suites = cipher_suites_pq_tls_1_0_2021_05_19,
    .allow_chacha20_boosting = false,
};

/* Same as ELBSecurityPolicy-TLS-1-1-2017-01, but with TLS 1.3 and PQ Ciphers appended to top of preference list */
struct s2n_cipher_suite *cipher_suites_pq_tls_1_1_2021_05_21[] = {
    /* TLS 1.3 Ciphers don't specify their Key exchange method, allowing for Hybrid PQ KEMs to be negotiated separately */
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_kyber_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_pq_tls_1_1_2021_05_21 = {
    .count = s2n_array_len(cipher_suites_pq_tls_1_1_2021_05_21),
    .suites = cipher_suites_pq_tls_1_1_2021_05_21,
    .allow_chacha20_boosting = false,
};

/* Same as cipher_preferences_20190214, but with TLS 1.3 and PQ Ciphers appended to top of preference list */
struct s2n_cipher_suite *cipher_suites_pq_tls_1_0_2021_05_22[] = {
    /* TLS 1.3 Ciphers don't specify their Key exchange method, allowing for Hybrid PQ KEMs to be negotiated separately */
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_kyber_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_gcm_sha256,
    &s2n_dhe_rsa_with_aes_256_gcm_sha384,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
};

const struct s2n_cipher_preferences cipher_preferences_pq_tls_1_0_2021_05_22 = {
    .count = s2n_array_len(cipher_suites_pq_tls_1_0_2021_05_22),
    .suites = cipher_suites_pq_tls_1_0_2021_05_22,
    .allow_chacha20_boosting = false,
};

/* Same as ELBSecurityPolicy-2016-08, but with TLS 1.3 and PQ Ciphers appended to top of preference list */
struct s2n_cipher_suite *cipher_suites_pq_tls_1_0_2021_05_23[] = {
    /* TLS 1.3 Ciphers don't specify their Key exchange method, allowing for Hybrid PQ KEMs to be negotiated separately */
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_kyber_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_pq_tls_1_0_2021_05_23 = {
    .count = s2n_array_len(cipher_suites_pq_tls_1_0_2021_05_23),
    .suites = cipher_suites_pq_tls_1_0_2021_05_23,
    .allow_chacha20_boosting = false,
};

/* Same as cipher_preferences_kms_pq_tls_1_0_2020_07, but with TLS 1.3 appended to top of preference list */
struct s2n_cipher_suite *cipher_suites_pq_tls_1_0_2021_05_24[] = {
    /* TLS 1.3 Ciphers don't specify their Key exchange method, allowing for Hybrid PQ KEMs to be negotiated separately */
    S2N_TLS13_CIPHER_SUITES_20190801,
    &s2n_ecdhe_kyber_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_3des_ede_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_pq_tls_1_0_2021_05_24 = {
    .count = s2n_array_len(cipher_suites_pq_tls_1_0_2021_05_24),
    .suites = cipher_suites_pq_tls_1_0_2021_05_24,
    .allow_chacha20_boosting = false,
};

/* Same as 20190214_gcm, but with PQ Ciphers appended to top of preference list */
struct s2n_cipher_suite *cipher_suites_pq_tls_1_0_2021_05_25[] = {
    &s2n_ecdhe_kyber_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_gcm_sha256,
    &s2n_dhe_rsa_with_aes_256_gcm_sha384,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
};

const struct s2n_cipher_preferences cipher_preferences_pq_tls_1_0_2021_05_25 = {
    .count = s2n_array_len(cipher_suites_pq_tls_1_0_2021_05_25),
    .suites = cipher_suites_pq_tls_1_0_2021_05_25,
    .allow_chacha20_boosting = false,
};

/* Same as 20190214_gcm, but with TLS 1.3 and PQ Ciphers appended to top of preference list */
struct s2n_cipher_suite *cipher_suites_pq_tls_1_0_2021_05_26[] = {
    /* TLS 1.3 Ciphers don't specify their Key exchange method, allowing for Hybrid PQ KEMs to be negotiated separately */
    S2N_TLS13_CLOUDFRONT_CIPHER_SUITES_20200716,
    &s2n_ecdhe_kyber_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_gcm_sha256,
    &s2n_dhe_rsa_with_aes_256_gcm_sha384,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
};

const struct s2n_cipher_preferences cipher_preferences_pq_tls_1_0_2021_05_26 = {
    .count = s2n_array_len(cipher_suites_pq_tls_1_0_2021_05_26),
    .suites = cipher_suites_pq_tls_1_0_2021_05_26,
    .allow_chacha20_boosting = false,
};

/* Same as 2021_05_26 except:
 *
 * 1. TLSv1.2 Kyber KEM cipher suites are removed
 * 2. AES 256 is preferred for TLS 1.3
 * 3. AES 128 is preferred for TLS 1.2 which has no PQ support in PQ-TLS-1-3-2023-06-01
 */
struct s2n_cipher_suite *cipher_suites_pq_tls_1_3_2023_06_01[] = {
    S2N_TLS13_CIPHER_SUITES_20190801,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_3des_ede_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_gcm_sha256,
    &s2n_dhe_rsa_with_aes_256_gcm_sha384,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
};

const struct s2n_cipher_preferences cipher_preferences_pq_tls_1_3_2023_06_01 = {
    .count = s2n_array_len(cipher_suites_pq_tls_1_3_2023_06_01),
    .suites = cipher_suites_pq_tls_1_3_2023_06_01,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_20231213[] = {
    &s2n_tls13_aes_128_gcm_sha256,
    &s2n_tls13_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_cbc_sha256,
};

const struct s2n_cipher_preferences cipher_preferences_20231213 = {
    .count = s2n_array_len(cipher_suites_20231213),
    .suites = cipher_suites_20231213,
};

struct s2n_cipher_suite *cipher_suites_20231214[] = {
    &s2n_tls13_aes_128_gcm_sha256,
    &s2n_tls13_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
};

const struct s2n_cipher_preferences cipher_preferences_20231214 = {
        .count = s2n_array_len(cipher_suites_20231214),
        .suites = cipher_suites_20231214,
};

struct s2n_cipher_suite *cipher_suites_kms_fips_tls_1_2_2018_10[] = {
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
};

const struct s2n_cipher_preferences cipher_preferences_kms_fips_tls_1_2_2018_10 = {
    .count = s2n_array_len(cipher_suites_kms_fips_tls_1_2_2018_10),
    .suites = cipher_suites_kms_fips_tls_1_2_2018_10,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_kms_fips_tls_1_2_2021_08[] = {
    &s2n_tls13_aes_128_gcm_sha256,
    &s2n_tls13_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
};

const struct s2n_cipher_preferences cipher_preferences_kms_fips_tls_1_2_2021_08 = {
    .count = s2n_array_len(cipher_suites_kms_fips_tls_1_2_2021_08),
    .suites = cipher_suites_kms_fips_tls_1_2_2021_08,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_20210816[] = {
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
};

const struct s2n_cipher_preferences cipher_preferences_20210816 = {
    .count = s2n_array_len(cipher_suites_20210816),
    .suites = cipher_suites_20210816,
    .allow_chacha20_boosting = false,
};

struct s2n_cipher_suite *cipher_suites_20210816_gcm[] = {
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
};

const struct s2n_cipher_preferences cipher_preferences_20210816_gcm = {
    .count = s2n_array_len(cipher_suites_20210816_gcm),
    .suites = cipher_suites_20210816_gcm,
    .allow_chacha20_boosting = false,
};

/* Cipher suite options for backwards compatibility with older clients,
 * while prioritizing forward secret key exchange and ECDSA certificates.
 */
struct s2n_cipher_suite *cipher_suites_20240603[] = {
    /* TLS1.3 suites */
    &s2n_tls13_aes_128_gcm_sha256,
    &s2n_tls13_aes_256_gcm_sha384,
    &s2n_tls13_chacha20_poly1305_sha256,

    /* Preferred ECDHE + ECDSA suites */
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_ecdsa_with_chacha20_poly1305_sha256,

    /* Preferred ECDHE + RSA suites */
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,
    &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256,

    /* Legacy ECDHE suites */
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,

    /* DHE suites */
    &s2n_dhe_rsa_with_aes_128_gcm_sha256,
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,
    &s2n_dhe_rsa_with_aes_256_gcm_sha384,
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,
    &s2n_dhe_rsa_with_aes_128_cbc_sha,
    &s2n_dhe_rsa_with_aes_256_cbc_sha,

    /* 3DES suites */
    &s2n_ecdhe_rsa_with_3des_ede_cbc_sha,
    &s2n_dhe_rsa_with_3des_ede_cbc_sha,

    /* RSA kex suites */
    &s2n_rsa_with_aes_128_gcm_sha256,
    &s2n_rsa_with_aes_128_cbc_sha256,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_256_cbc_sha256,
    &s2n_rsa_with_aes_128_cbc_sha,
    &s2n_rsa_with_aes_256_cbc_sha,
    &s2n_rsa_with_3des_ede_cbc_sha,
};

const struct s2n_cipher_preferences cipher_preferences_20240603 = {
    .count = s2n_array_len(cipher_suites_20240603),
    .suites = cipher_suites_20240603,
    .allow_chacha20_boosting = true,
};

struct s2n_cipher_suite *cipher_suites_rfc9151[] = {
    /* TLS1.2 */
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,
    &s2n_rsa_with_aes_256_gcm_sha384,
    &s2n_dhe_rsa_with_aes_256_gcm_sha384,

    /* TLS1.3 */
    &s2n_tls13_aes_256_gcm_sha384,
};

const struct s2n_cipher_preferences cipher_preferences_rfc9151 = {
    .count = s2n_array_len(cipher_suites_rfc9151),
    .suites = cipher_suites_rfc9151,
    .allow_chacha20_boosting = false,
};

/* clang-format on */
