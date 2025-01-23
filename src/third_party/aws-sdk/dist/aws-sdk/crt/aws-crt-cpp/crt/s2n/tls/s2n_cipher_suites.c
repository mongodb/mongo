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

#include <openssl/crypto.h>
#include <string.h>

#include "crypto/s2n_cipher.h"
#include "crypto/s2n_openssl.h"
#include "crypto/s2n_pq.h"
#include "error/s2n_errno.h"
#include "tls/s2n_auth_selection.h"
#include "tls/s2n_kex.h"
#include "tls/s2n_psk.h"
#include "tls/s2n_security_policies.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls13.h"
#include "utils/s2n_safety.h"

/*************************
 * S2n Record Algorithms *
 *************************/
const struct s2n_record_algorithm s2n_record_alg_null = {
    .cipher = &s2n_null_cipher,
    .hmac_alg = S2N_HMAC_NONE,
    .flags = 0,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_rc4_md5 = {
    .cipher = &s2n_rc4,
    .hmac_alg = S2N_HMAC_MD5,
    .flags = 0,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_rc4_sslv3_md5 = {
    .cipher = &s2n_rc4,
    .hmac_alg = S2N_HMAC_SSLv3_MD5,
    .flags = 0,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_rc4_sha = {
    .cipher = &s2n_rc4,
    .hmac_alg = S2N_HMAC_SHA1,
    .flags = 0,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_rc4_sslv3_sha = {
    .cipher = &s2n_rc4,
    .hmac_alg = S2N_HMAC_SSLv3_SHA1,
    .flags = 0,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_3des_sha = {
    .cipher = &s2n_3des,
    .hmac_alg = S2N_HMAC_SHA1,
    .flags = 0,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_3des_sslv3_sha = {
    .cipher = &s2n_3des,
    .hmac_alg = S2N_HMAC_SSLv3_SHA1,
    .flags = 0,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_aes128_sha = {
    .cipher = &s2n_aes128,
    .hmac_alg = S2N_HMAC_SHA1,
    .flags = 0,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_aes128_sslv3_sha = {
    .cipher = &s2n_aes128,
    .hmac_alg = S2N_HMAC_SSLv3_SHA1,
    .flags = 0,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_aes128_sha_composite = {
    .cipher = &s2n_aes128_sha,
    .hmac_alg = S2N_HMAC_NONE,
    .flags = 0,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_aes128_sha256 = {
    .cipher = &s2n_aes128,
    .hmac_alg = S2N_HMAC_SHA256,
    .flags = 0,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_aes128_sha256_composite = {
    .cipher = &s2n_aes128_sha256,
    .hmac_alg = S2N_HMAC_NONE,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_aes256_sha = {
    .cipher = &s2n_aes256,
    .hmac_alg = S2N_HMAC_SHA1,
    .flags = 0,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_aes256_sslv3_sha = {
    .cipher = &s2n_aes256,
    .hmac_alg = S2N_HMAC_SSLv3_SHA1,
    .flags = 0,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_aes256_sha_composite = {
    .cipher = &s2n_aes256_sha,
    .hmac_alg = S2N_HMAC_NONE,
    .flags = 0,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_aes256_sha256 = {
    .cipher = &s2n_aes256,
    .hmac_alg = S2N_HMAC_SHA256,
    .flags = 0,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_aes256_sha256_composite = {
    .cipher = &s2n_aes256_sha256,
    .hmac_alg = S2N_HMAC_NONE,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_aes256_sha384 = {
    .cipher = &s2n_aes256,
    .hmac_alg = S2N_HMAC_SHA384,
    .flags = 0,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_aes128_gcm = {
    .cipher = &s2n_aes128_gcm,
    .hmac_alg = S2N_HMAC_NONE,
    .flags = S2N_TLS12_AES_GCM_AEAD_NONCE,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_aes256_gcm = {
    .cipher = &s2n_aes256_gcm,
    .hmac_alg = S2N_HMAC_NONE,
    .flags = S2N_TLS12_AES_GCM_AEAD_NONCE,
    .encryption_limit = UINT64_MAX,
};

const struct s2n_record_algorithm s2n_record_alg_chacha20_poly1305 = {
    .cipher = &s2n_chacha20_poly1305,
    .hmac_alg = S2N_HMAC_NONE,
    /* Per RFC 7905, ChaCha20-Poly1305 will use a nonce construction expected to be used in TLS1.3.
     * Give it a distinct 1.2 nonce value in case this changes.
     */
    .flags = S2N_TLS12_CHACHA_POLY_AEAD_NONCE,
    .encryption_limit = UINT64_MAX,
};

/* TLS 1.3 Record Algorithms */
const struct s2n_record_algorithm s2n_tls13_record_alg_aes128_gcm = {
    .cipher = &s2n_tls13_aes128_gcm,
    .hmac_alg = S2N_HMAC_NONE, /* previously used in 1.2 prf, we do not need this */
    .flags = S2N_TLS13_RECORD_AEAD_NONCE,
    .encryption_limit = S2N_TLS13_AES_GCM_MAXIMUM_RECORD_NUMBER,
};

const struct s2n_record_algorithm s2n_tls13_record_alg_aes256_gcm = {
    .cipher = &s2n_tls13_aes256_gcm,
    .hmac_alg = S2N_HMAC_NONE,
    .flags = S2N_TLS13_RECORD_AEAD_NONCE,
    .encryption_limit = S2N_TLS13_AES_GCM_MAXIMUM_RECORD_NUMBER,
};

const struct s2n_record_algorithm s2n_tls13_record_alg_chacha20_poly1305 = {
    .cipher = &s2n_chacha20_poly1305,
    .hmac_alg = S2N_HMAC_NONE,
    /* this mirrors s2n_record_alg_chacha20_poly1305 with the exception of TLS 1.3 nonce flag */
    .flags = S2N_TLS13_RECORD_AEAD_NONCE,
    .encryption_limit = UINT64_MAX,
};

/*********************
 * S2n Cipher Suites *
 *********************/

/* This is the initial cipher suite, but is never negotiated */
struct s2n_cipher_suite s2n_null_cipher_suite = {
    .available = 1,
    .name = "TLS_NULL_WITH_NULL_NULL",
    .iana_name = "TLS_NULL_WITH_NULL_NULL",
    .iana_value = { TLS_NULL_WITH_NULL_NULL },
    .key_exchange_alg = &s2n_rsa,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = &s2n_record_alg_null,
};

struct s2n_cipher_suite s2n_rsa_with_rc4_128_md5 = /* 0x00,0x04 */ {
    .available = 0,
    .name = "RC4-MD5",
    .iana_name = "TLS_RSA_WITH_RC4_128_MD5",
    .iana_value = { TLS_RSA_WITH_RC4_128_MD5 },
    .key_exchange_alg = &s2n_rsa,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_rc4_md5 },
    .num_record_algs = 1,
    .sslv3_record_alg = &s2n_record_alg_rc4_sslv3_md5,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_SSLv3,
};

struct s2n_cipher_suite s2n_rsa_with_rc4_128_sha = /* 0x00,0x05 */ {
    .available = 0,
    .name = "RC4-SHA",
    .iana_name = "TLS_RSA_WITH_RC4_128_SHA",
    .iana_value = { TLS_RSA_WITH_RC4_128_SHA },
    .key_exchange_alg = &s2n_rsa,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_rc4_sha },
    .num_record_algs = 1,
    .sslv3_record_alg = &s2n_record_alg_rc4_sslv3_sha,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_SSLv3,
};

struct s2n_cipher_suite s2n_rsa_with_3des_ede_cbc_sha = /* 0x00,0x0A */ {
    .available = 0,
    .name = "DES-CBC3-SHA",
    .iana_name = "TLS_RSA_WITH_3DES_EDE_CBC_SHA",
    .iana_value = { TLS_RSA_WITH_3DES_EDE_CBC_SHA },
    .key_exchange_alg = &s2n_rsa,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_3des_sha },
    .num_record_algs = 1,
    .sslv3_record_alg = &s2n_record_alg_3des_sslv3_sha,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_SSLv3,
};

struct s2n_cipher_suite s2n_dhe_rsa_with_3des_ede_cbc_sha = /* 0x00,0x16 */ {
    .available = 0,
    .name = "DHE-RSA-DES-CBC3-SHA",
    .iana_name = "TLS_DHE_RSA_WITH_3DES_EDE_CBC_SHA",
    .iana_value = { TLS_DHE_RSA_WITH_3DES_EDE_CBC_SHA },
    .key_exchange_alg = &s2n_dhe,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_3des_sha },
    .num_record_algs = 1,
    .sslv3_record_alg = &s2n_record_alg_3des_sslv3_sha,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_SSLv3,
};

struct s2n_cipher_suite s2n_rsa_with_aes_128_cbc_sha = /* 0x00,0x2F */ {
    .available = 0,
    .name = "AES128-SHA",
    .iana_name = "TLS_RSA_WITH_AES_128_CBC_SHA",
    .iana_value = { TLS_RSA_WITH_AES_128_CBC_SHA },
    .key_exchange_alg = &s2n_rsa,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes128_sha_composite, &s2n_record_alg_aes128_sha },
    .num_record_algs = 2,
    .sslv3_record_alg = &s2n_record_alg_aes128_sslv3_sha,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_SSLv3,
};

struct s2n_cipher_suite s2n_dhe_rsa_with_aes_128_cbc_sha = /* 0x00,0x33 */ {
    .available = 0,
    .name = "DHE-RSA-AES128-SHA",
    .iana_name = "TLS_DHE_RSA_WITH_AES_128_CBC_SHA",
    .iana_value = { TLS_DHE_RSA_WITH_AES_128_CBC_SHA },
    .key_exchange_alg = &s2n_dhe,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes128_sha_composite, &s2n_record_alg_aes128_sha },
    .num_record_algs = 2,
    .sslv3_record_alg = &s2n_record_alg_aes128_sslv3_sha,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_SSLv3,
};

struct s2n_cipher_suite s2n_rsa_with_aes_256_cbc_sha = /* 0x00,0x35 */ {
    .available = 0,
    .name = "AES256-SHA",
    .iana_name = "TLS_RSA_WITH_AES_256_CBC_SHA",
    .iana_value = { TLS_RSA_WITH_AES_256_CBC_SHA },
    .key_exchange_alg = &s2n_rsa,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes256_sha_composite, &s2n_record_alg_aes256_sha },
    .num_record_algs = 2,
    .sslv3_record_alg = &s2n_record_alg_aes256_sslv3_sha,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_SSLv3,
};

struct s2n_cipher_suite s2n_dhe_rsa_with_aes_256_cbc_sha = /* 0x00,0x39 */ {
    .available = 0,
    .name = "DHE-RSA-AES256-SHA",
    .iana_name = "TLS_DHE_RSA_WITH_AES_256_CBC_SHA",
    .iana_value = { TLS_DHE_RSA_WITH_AES_256_CBC_SHA },
    .key_exchange_alg = &s2n_dhe,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes256_sha_composite, &s2n_record_alg_aes256_sha },
    .num_record_algs = 2,
    .sslv3_record_alg = &s2n_record_alg_aes256_sslv3_sha,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_SSLv3,
};

struct s2n_cipher_suite s2n_rsa_with_aes_128_cbc_sha256 = /* 0x00,0x3C */ {
    .available = 0,
    .name = "AES128-SHA256",
    .iana_name = "TLS_RSA_WITH_AES_128_CBC_SHA256",
    .iana_value = { TLS_RSA_WITH_AES_128_CBC_SHA256 },
    .key_exchange_alg = &s2n_rsa,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes128_sha256_composite, &s2n_record_alg_aes128_sha256 },
    .num_record_algs = 2,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_rsa_with_aes_256_cbc_sha256 = /* 0x00,0x3D */ {
    .available = 0,
    .name = "AES256-SHA256",
    .iana_name = "TLS_RSA_WITH_AES_256_CBC_SHA256",
    .iana_value = { TLS_RSA_WITH_AES_256_CBC_SHA256 },
    .key_exchange_alg = &s2n_rsa,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes256_sha256_composite, &s2n_record_alg_aes256_sha256 },
    .num_record_algs = 2,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_dhe_rsa_with_aes_128_cbc_sha256 = /* 0x00,0x67 */ {
    .available = 0,
    .name = "DHE-RSA-AES128-SHA256",
    .iana_name = "TLS_DHE_RSA_WITH_AES_128_CBC_SHA256",
    .iana_value = { TLS_DHE_RSA_WITH_AES_128_CBC_SHA256 },
    .key_exchange_alg = &s2n_dhe,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes128_sha256_composite, &s2n_record_alg_aes128_sha256 },
    .num_record_algs = 2,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_dhe_rsa_with_aes_256_cbc_sha256 = /* 0x00,0x6B */ {
    .available = 0,
    .name = "DHE-RSA-AES256-SHA256",
    .iana_name = "TLS_DHE_RSA_WITH_AES_256_CBC_SHA256",
    .iana_value = { TLS_DHE_RSA_WITH_AES_256_CBC_SHA256 },
    .key_exchange_alg = &s2n_dhe,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes256_sha256_composite, &s2n_record_alg_aes256_sha256 },
    .num_record_algs = 2,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_rsa_with_aes_128_gcm_sha256 = /* 0x00,0x9C */ {
    .available = 0,
    .name = "AES128-GCM-SHA256",
    .iana_name = "TLS_RSA_WITH_AES_128_GCM_SHA256",
    .iana_value = { TLS_RSA_WITH_AES_128_GCM_SHA256 },
    .key_exchange_alg = &s2n_rsa,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes128_gcm },
    .num_record_algs = 1,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_rsa_with_aes_256_gcm_sha384 = /* 0x00,0x9D */ {
    .available = 0,
    .name = "AES256-GCM-SHA384",
    .iana_name = "TLS_RSA_WITH_AES_256_GCM_SHA384",
    .iana_value = { TLS_RSA_WITH_AES_256_GCM_SHA384 },
    .key_exchange_alg = &s2n_rsa,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes256_gcm },
    .num_record_algs = 1,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA384,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_dhe_rsa_with_aes_128_gcm_sha256 = /* 0x00,0x9E */ {
    .available = 0,
    .name = "DHE-RSA-AES128-GCM-SHA256",
    .iana_name = "TLS_DHE_RSA_WITH_AES_128_GCM_SHA256",
    .iana_value = { TLS_DHE_RSA_WITH_AES_128_GCM_SHA256 },
    .key_exchange_alg = &s2n_dhe,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes128_gcm },
    .num_record_algs = 1,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_dhe_rsa_with_aes_256_gcm_sha384 = /* 0x00,0x9F */ {
    .available = 0,
    .name = "DHE-RSA-AES256-GCM-SHA384",
    .iana_name = "TLS_DHE_RSA_WITH_AES_256_GCM_SHA384",
    .iana_value = { TLS_DHE_RSA_WITH_AES_256_GCM_SHA384 },
    .key_exchange_alg = &s2n_dhe,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes256_gcm },
    .num_record_algs = 1,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA384,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_ecdhe_ecdsa_with_aes_128_cbc_sha = /* 0xC0,0x09 */ {
    .available = 0,
    .name = "ECDHE-ECDSA-AES128-SHA",
    .iana_name = "TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA",
    .iana_value = { TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA },
    .key_exchange_alg = &s2n_ecdhe,
    .auth_method = S2N_AUTHENTICATION_ECDSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes128_sha_composite, &s2n_record_alg_aes128_sha },
    .num_record_algs = 2,
    .sslv3_record_alg = &s2n_record_alg_aes128_sslv3_sha,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_SSLv3,
};

struct s2n_cipher_suite s2n_ecdhe_ecdsa_with_aes_256_cbc_sha = /* 0xC0,0x0A */ {
    .available = 0,
    .name = "ECDHE-ECDSA-AES256-SHA",
    .iana_name = "TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA",
    .iana_value = { TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA },
    .key_exchange_alg = &s2n_ecdhe,
    .auth_method = S2N_AUTHENTICATION_ECDSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes256_sha_composite, &s2n_record_alg_aes256_sha },
    .num_record_algs = 2,
    .sslv3_record_alg = &s2n_record_alg_aes256_sslv3_sha,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_SSLv3,
};

struct s2n_cipher_suite s2n_ecdhe_rsa_with_rc4_128_sha = /* 0xC0,0x11 */ {
    .available = 0,
    .name = "ECDHE-RSA-RC4-SHA",
    .iana_name = "TLS_ECDHE_RSA_WITH_RC4_128_SHA",
    .iana_value = { TLS_ECDHE_RSA_WITH_RC4_128_SHA },
    .key_exchange_alg = &s2n_ecdhe,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_rc4_sha },
    .num_record_algs = 1,
    .sslv3_record_alg = &s2n_record_alg_rc4_sslv3_sha,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_SSLv3,
};

struct s2n_cipher_suite s2n_ecdhe_rsa_with_3des_ede_cbc_sha = /* 0xC0,0x12 */ {
    .available = 0,
    .name = "ECDHE-RSA-DES-CBC3-SHA",
    .iana_name = "TLS_ECDHE_RSA_WITH_3DES_EDE_CBC_SHA",
    .iana_value = { TLS_ECDHE_RSA_WITH_3DES_EDE_CBC_SHA },
    .key_exchange_alg = &s2n_ecdhe,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_3des_sha },
    .num_record_algs = 1,
    .sslv3_record_alg = &s2n_record_alg_3des_sslv3_sha,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_SSLv3,
};

struct s2n_cipher_suite s2n_ecdhe_rsa_with_aes_128_cbc_sha = /* 0xC0,0x13 */ {
    .available = 0,
    .name = "ECDHE-RSA-AES128-SHA",
    .iana_name = "TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA",
    .iana_value = { TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA },
    .key_exchange_alg = &s2n_ecdhe,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes128_sha_composite, &s2n_record_alg_aes128_sha },
    .num_record_algs = 2,
    .sslv3_record_alg = &s2n_record_alg_aes128_sslv3_sha,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_SSLv3,
};

struct s2n_cipher_suite s2n_ecdhe_rsa_with_aes_256_cbc_sha = /* 0xC0,0x14 */ {
    .available = 0,
    .name = "ECDHE-RSA-AES256-SHA",
    .iana_name = "TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA",
    .iana_value = { TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA },
    .key_exchange_alg = &s2n_ecdhe,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes256_sha_composite, &s2n_record_alg_aes256_sha },
    .num_record_algs = 2,
    .sslv3_record_alg = &s2n_record_alg_aes256_sslv3_sha,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_SSLv3,
};

struct s2n_cipher_suite s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256 = /* 0xC0,0x23 */ {
    .available = 0,
    .name = "ECDHE-ECDSA-AES128-SHA256",
    .iana_name = "TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256",
    .iana_value = { TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256 },
    .key_exchange_alg = &s2n_ecdhe,
    .auth_method = S2N_AUTHENTICATION_ECDSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes128_sha256_composite, &s2n_record_alg_aes128_sha256 },
    .num_record_algs = 2,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384 = /* 0xC0,0x24 */ {
    .available = 0,
    .name = "ECDHE-ECDSA-AES256-SHA384",
    .iana_name = "TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384",
    .iana_value = { TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384 },
    .key_exchange_alg = &s2n_ecdhe,
    .auth_method = S2N_AUTHENTICATION_ECDSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes256_sha384 },
    .num_record_algs = 1,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA384,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_ecdhe_rsa_with_aes_128_cbc_sha256 = /* 0xC0,0x27 */ {
    .available = 0,
    .name = "ECDHE-RSA-AES128-SHA256",
    .iana_name = "TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256",
    .iana_value = { TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256 },
    .key_exchange_alg = &s2n_ecdhe,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes128_sha256_composite, &s2n_record_alg_aes128_sha256 },
    .num_record_algs = 2,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_ecdhe_rsa_with_aes_256_cbc_sha384 = /* 0xC0,0x28 */ {
    .available = 0,
    .name = "ECDHE-RSA-AES256-SHA384",
    .iana_name = "TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384",
    .iana_value = { TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384 },
    .key_exchange_alg = &s2n_ecdhe,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes256_sha384 },
    .num_record_algs = 1,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA384,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256 = /* 0xC0,0x2B */ {
    .available = 0,
    .name = "ECDHE-ECDSA-AES128-GCM-SHA256",
    .iana_name = "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256",
    .iana_value = { TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 },
    .key_exchange_alg = &s2n_ecdhe,
    .auth_method = S2N_AUTHENTICATION_ECDSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes128_gcm },
    .num_record_algs = 1,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384 = /* 0xC0,0x2C */ {
    .available = 0,
    .name = "ECDHE-ECDSA-AES256-GCM-SHA384",
    .iana_name = "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384",
    .iana_value = { TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384 },
    .key_exchange_alg = &s2n_ecdhe,
    .auth_method = S2N_AUTHENTICATION_ECDSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes256_gcm },
    .num_record_algs = 1,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA384,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_ecdhe_rsa_with_aes_128_gcm_sha256 = /* 0xC0,0x2F */ {
    .available = 0,
    .name = "ECDHE-RSA-AES128-GCM-SHA256",
    .iana_name = "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256",
    .iana_value = { TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 },
    .key_exchange_alg = &s2n_ecdhe,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes128_gcm },
    .num_record_algs = 1,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_ecdhe_rsa_with_aes_256_gcm_sha384 = /* 0xC0,0x30 */ {
    .available = 0,
    .name = "ECDHE-RSA-AES256-GCM-SHA384",
    .iana_name = "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384",
    .iana_value = { TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384 },
    .key_exchange_alg = &s2n_ecdhe,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes256_gcm },
    .num_record_algs = 1,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA384,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_ecdhe_rsa_with_chacha20_poly1305_sha256 = /* 0xCC,0xA8 */ {
    .available = 0,
    .name = "ECDHE-RSA-CHACHA20-POLY1305",
    .iana_name = "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256",
    .iana_value = { TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256 },
    .key_exchange_alg = &s2n_ecdhe,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_chacha20_poly1305 },
    .num_record_algs = 1,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_ecdhe_ecdsa_with_chacha20_poly1305_sha256 = /* 0xCC,0xA9 */ {
    .available = 0,
    .name = "ECDHE-ECDSA-CHACHA20-POLY1305",
    .iana_name = "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256",
    .iana_value = { TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256 },
    .key_exchange_alg = &s2n_ecdhe,
    .auth_method = S2N_AUTHENTICATION_ECDSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_chacha20_poly1305 },
    .num_record_algs = 1,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_dhe_rsa_with_chacha20_poly1305_sha256 = /* 0xCC,0xAA */ {
    .available = 0,
    .name = "DHE-RSA-CHACHA20-POLY1305",
    .iana_name = "TLS_DHE_RSA_WITH_CHACHA20_POLY1305_SHA256",
    .iana_value = { TLS_DHE_RSA_WITH_CHACHA20_POLY1305_SHA256 },
    .key_exchange_alg = &s2n_dhe,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_chacha20_poly1305 },
    .num_record_algs = 1,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_TLS12,
};

/* From https://tools.ietf.org/html/draft-campagna-tls-bike-sike-hybrid */

struct s2n_cipher_suite s2n_ecdhe_kyber_rsa_with_aes_256_gcm_sha384 = /* 0xFF, 0x0C */ {
    .available = 0,
    .name = "ECDHE-KYBER-RSA-AES256-GCM-SHA384",
    .iana_name = "TLS_ECDHE_KYBER_RSA_WITH_AES_256_GCM_SHA384",
    .iana_value = { TLS_ECDHE_KYBER_RSA_WITH_AES_256_GCM_SHA384 },
    .key_exchange_alg = &s2n_hybrid_ecdhe_kem,
    .auth_method = S2N_AUTHENTICATION_RSA,
    .record_alg = NULL,
    .all_record_algs = { &s2n_record_alg_aes256_gcm },
    .num_record_algs = 1,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA384,
    .minimum_required_tls_version = S2N_TLS12,
};

struct s2n_cipher_suite s2n_tls13_aes_128_gcm_sha256 = {
    .available = 0,
    .name = "TLS_AES_128_GCM_SHA256",
    .iana_name = "TLS_AES_128_GCM_SHA256",
    .iana_value = { TLS_AES_128_GCM_SHA256 },
    .key_exchange_alg = &s2n_tls13_kex,
    .auth_method = S2N_AUTHENTICATION_METHOD_TLS13,
    .record_alg = NULL,
    .all_record_algs = { &s2n_tls13_record_alg_aes128_gcm },
    .num_record_algs = 1,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_TLS13,
};

struct s2n_cipher_suite s2n_tls13_aes_256_gcm_sha384 = {
    .available = 0,
    .name = "TLS_AES_256_GCM_SHA384",
    .iana_name = "TLS_AES_256_GCM_SHA384",
    .iana_value = { TLS_AES_256_GCM_SHA384 },
    .key_exchange_alg = &s2n_tls13_kex,
    .auth_method = S2N_AUTHENTICATION_METHOD_TLS13,
    .record_alg = NULL,
    .all_record_algs = { &s2n_tls13_record_alg_aes256_gcm },
    .num_record_algs = 1,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA384,
    .minimum_required_tls_version = S2N_TLS13,
};

struct s2n_cipher_suite s2n_tls13_chacha20_poly1305_sha256 = {
    .available = 0,
    .name = "TLS_CHACHA20_POLY1305_SHA256",
    .iana_name = "TLS_CHACHA20_POLY1305_SHA256",
    .iana_value = { TLS_CHACHA20_POLY1305_SHA256 },
    .key_exchange_alg = &s2n_tls13_kex,
    .auth_method = S2N_AUTHENTICATION_METHOD_TLS13,
    .record_alg = NULL,
    .all_record_algs = { &s2n_tls13_record_alg_chacha20_poly1305 },
    .num_record_algs = 1,
    .sslv3_record_alg = NULL,
    .prf_alg = S2N_HMAC_SHA256,
    .minimum_required_tls_version = S2N_TLS13,
};

/* All of the cipher suites that s2n negotiates in order of IANA value.
 * New cipher suites MUST be added here, IN ORDER, or they will not be
 * properly initialized.
 */
static struct s2n_cipher_suite *s2n_all_cipher_suites[] = {
    &s2n_rsa_with_rc4_128_md5,            /* 0x00,0x04 */
    &s2n_rsa_with_rc4_128_sha,            /* 0x00,0x05 */
    &s2n_rsa_with_3des_ede_cbc_sha,       /* 0x00,0x0A */
    &s2n_dhe_rsa_with_3des_ede_cbc_sha,   /* 0x00,0x16 */
    &s2n_rsa_with_aes_128_cbc_sha,        /* 0x00,0x2F */
    &s2n_dhe_rsa_with_aes_128_cbc_sha,    /* 0x00,0x33 */
    &s2n_rsa_with_aes_256_cbc_sha,        /* 0x00,0x35 */
    &s2n_dhe_rsa_with_aes_256_cbc_sha,    /* 0x00,0x39 */
    &s2n_rsa_with_aes_128_cbc_sha256,     /* 0x00,0x3C */
    &s2n_rsa_with_aes_256_cbc_sha256,     /* 0x00,0x3D */
    &s2n_dhe_rsa_with_aes_128_cbc_sha256, /* 0x00,0x67 */
    &s2n_dhe_rsa_with_aes_256_cbc_sha256, /* 0x00,0x6B */
    &s2n_rsa_with_aes_128_gcm_sha256,     /* 0x00,0x9C */
    &s2n_rsa_with_aes_256_gcm_sha384,     /* 0x00,0x9D */
    &s2n_dhe_rsa_with_aes_128_gcm_sha256, /* 0x00,0x9E */
    &s2n_dhe_rsa_with_aes_256_gcm_sha384, /* 0x00,0x9F */

    &s2n_tls13_aes_128_gcm_sha256,       /* 0x13,0x01 */
    &s2n_tls13_aes_256_gcm_sha384,       /* 0x13,0x02 */
    &s2n_tls13_chacha20_poly1305_sha256, /* 0x13,0x03 */

    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,          /* 0xC0,0x09 */
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,          /* 0xC0,0x0A */
    &s2n_ecdhe_rsa_with_rc4_128_sha,                /* 0xC0,0x11 */
    &s2n_ecdhe_rsa_with_3des_ede_cbc_sha,           /* 0xC0,0x12 */
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,            /* 0xC0,0x13 */
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,            /* 0xC0,0x14 */
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,       /* 0xC0,0x23 */
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,       /* 0xC0,0x24 */
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,         /* 0xC0,0x27 */
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,         /* 0xC0,0x28 */
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,       /* 0xC0,0x2B */
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,       /* 0xC0,0x2C */
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,         /* 0xC0,0x2F */
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,         /* 0xC0,0x30 */
    &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256,   /* 0xCC,0xA8 */
    &s2n_ecdhe_ecdsa_with_chacha20_poly1305_sha256, /* 0xCC,0xA9 */
    &s2n_dhe_rsa_with_chacha20_poly1305_sha256,     /* 0xCC,0xAA */
    &s2n_ecdhe_kyber_rsa_with_aes_256_gcm_sha384,   /* 0xFF,0x0C */
};

/* All supported ciphers. Exposed for integration testing. */
const struct s2n_cipher_preferences cipher_preferences_test_all = {
    .count = s2n_array_len(s2n_all_cipher_suites),
    .suites = s2n_all_cipher_suites,
};

/* All TLS12 Cipher Suites */

static struct s2n_cipher_suite *s2n_all_tls12_cipher_suites[] = {
    &s2n_rsa_with_rc4_128_md5,            /* 0x00,0x04 */
    &s2n_rsa_with_rc4_128_sha,            /* 0x00,0x05 */
    &s2n_rsa_with_3des_ede_cbc_sha,       /* 0x00,0x0A */
    &s2n_dhe_rsa_with_3des_ede_cbc_sha,   /* 0x00,0x16 */
    &s2n_rsa_with_aes_128_cbc_sha,        /* 0x00,0x2F */
    &s2n_dhe_rsa_with_aes_128_cbc_sha,    /* 0x00,0x33 */
    &s2n_rsa_with_aes_256_cbc_sha,        /* 0x00,0x35 */
    &s2n_dhe_rsa_with_aes_256_cbc_sha,    /* 0x00,0x39 */
    &s2n_rsa_with_aes_128_cbc_sha256,     /* 0x00,0x3C */
    &s2n_rsa_with_aes_256_cbc_sha256,     /* 0x00,0x3D */
    &s2n_dhe_rsa_with_aes_128_cbc_sha256, /* 0x00,0x67 */
    &s2n_dhe_rsa_with_aes_256_cbc_sha256, /* 0x00,0x6B */
    &s2n_rsa_with_aes_128_gcm_sha256,     /* 0x00,0x9C */
    &s2n_rsa_with_aes_256_gcm_sha384,     /* 0x00,0x9D */
    &s2n_dhe_rsa_with_aes_128_gcm_sha256, /* 0x00,0x9E */
    &s2n_dhe_rsa_with_aes_256_gcm_sha384, /* 0x00,0x9F */

    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,          /* 0xC0,0x09 */
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,          /* 0xC0,0x0A */
    &s2n_ecdhe_rsa_with_rc4_128_sha,                /* 0xC0,0x11 */
    &s2n_ecdhe_rsa_with_3des_ede_cbc_sha,           /* 0xC0,0x12 */
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,            /* 0xC0,0x13 */
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,            /* 0xC0,0x14 */
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,       /* 0xC0,0x23 */
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,       /* 0xC0,0x24 */
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,         /* 0xC0,0x27 */
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,         /* 0xC0,0x28 */
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,       /* 0xC0,0x2B */
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,       /* 0xC0,0x2C */
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,         /* 0xC0,0x2F */
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,         /* 0xC0,0x30 */
    &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256,   /* 0xCC,0xA8 */
    &s2n_ecdhe_ecdsa_with_chacha20_poly1305_sha256, /* 0xCC,0xA9 */
    &s2n_dhe_rsa_with_chacha20_poly1305_sha256,     /* 0xCC,0xAA */
    &s2n_ecdhe_kyber_rsa_with_aes_256_gcm_sha384,   /* 0xFF,0x0C */
};

const struct s2n_cipher_preferences cipher_preferences_test_all_tls12 = {
    .count = s2n_array_len(s2n_all_tls12_cipher_suites),
    .suites = s2n_all_tls12_cipher_suites,
};

/* All of the cipher suites that s2n can negotiate when in FIPS mode,
 * in order of IANA value. Exposed for the "test_all_fips" cipher preference list.
 */
static struct s2n_cipher_suite *s2n_all_fips_cipher_suites[] = {
    &s2n_dhe_rsa_with_aes_128_cbc_sha,        /* 0x00,0x33 */
    &s2n_dhe_rsa_with_aes_256_cbc_sha,        /* 0x00,0x39 */
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,     /* 0x00,0x67 */
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,     /* 0x00,0x6B */
    &s2n_dhe_rsa_with_aes_128_gcm_sha256,     /* 0x00,0x9E */
    &s2n_dhe_rsa_with_aes_256_gcm_sha384,     /* 0x00,0x9F */
    &s2n_tls13_aes_128_gcm_sha256,            /* 0x13,0x01 */
    &s2n_tls13_aes_256_gcm_sha384,            /* 0x13,0x02 */
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,    /* 0xC0,0x09 */
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,    /* 0xC0,0x0A */
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,      /* 0xC0,0x13 */
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,      /* 0xC0,0x14 */
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256, /* 0xC0,0x23 */
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384, /* 0xC0,0x24 */
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,   /* 0xC0,0x27 */
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,   /* 0xC0,0x28 */
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256, /* 0xC0,0x2B */
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384, /* 0xC0,0x2C */
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,   /* 0xC0,0x2F */
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,   /* 0xC0,0x30 */
};

/* All supported FIPS ciphers. Exposed for integration testing. */
const struct s2n_cipher_preferences cipher_preferences_test_all_fips = {
    .count = s2n_array_len(s2n_all_fips_cipher_suites),
    .suites = s2n_all_fips_cipher_suites,
};

/* All of the ECDSA cipher suites that s2n can negotiate, in order of IANA
 * value. Exposed for the "test_all_ecdsa" cipher preference list.
 */
static struct s2n_cipher_suite *s2n_all_ecdsa_cipher_suites[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,          /* 0xC0,0x09 */
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,          /* 0xC0,0x0A */
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,       /* 0xC0,0x23 */
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,       /* 0xC0,0x24 */
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,       /* 0xC0,0x2B */
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,       /* 0xC0,0x2C */
    &s2n_ecdhe_ecdsa_with_chacha20_poly1305_sha256, /* 0xCC,0xA9 */
};

/* All supported ECDSA cipher suites. Exposed for integration testing. */
const struct s2n_cipher_preferences cipher_preferences_test_all_ecdsa = {
    .count = s2n_array_len(s2n_all_ecdsa_cipher_suites),
    .suites = s2n_all_ecdsa_cipher_suites,
};

/* All cipher suites that uses RSA key exchange. Exposed for unit or integration tests. */
static struct s2n_cipher_suite *s2n_all_rsa_kex_cipher_suites[] = {
    &s2n_rsa_with_aes_128_cbc_sha,    /* 0x00,0x2F */
    &s2n_rsa_with_rc4_128_md5,        /* 0x00,0x04 */
    &s2n_rsa_with_rc4_128_sha,        /* 0x00,0x05 */
    &s2n_rsa_with_3des_ede_cbc_sha,   /* 0x00,0x0A */
    &s2n_rsa_with_aes_128_cbc_sha,    /* 0x00,0x2F */
    &s2n_rsa_with_aes_256_cbc_sha,    /* 0x00,0x35 */
    &s2n_rsa_with_aes_128_cbc_sha256, /* 0x00,0x3C */
    &s2n_rsa_with_aes_256_cbc_sha256, /* 0x00,0x3D */
    &s2n_rsa_with_aes_128_gcm_sha256, /* 0x00,0x9C */
    &s2n_rsa_with_aes_256_gcm_sha384, /* 0x00,0x9D */
};

/* Cipher preferences with rsa key exchange. Exposed for unit and integration tests. */
const struct s2n_cipher_preferences cipher_preferences_test_all_rsa_kex = {
    .count = s2n_array_len(s2n_all_rsa_kex_cipher_suites),
    .suites = s2n_all_rsa_kex_cipher_suites,
};

/* All ECDSA cipher suites first, then the rest of the supported ciphers that s2n can negotiate.
 * Exposed for the "test_ecdsa_priority" cipher preference list.
 */
static struct s2n_cipher_suite *s2n_ecdsa_priority_cipher_suites[] = {
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha,          /* 0xC0,0x09 */
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha,          /* 0xC0,0x0A */
    &s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256,       /* 0xC0,0x23 */
    &s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384,       /* 0xC0,0x24 */
    &s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256,       /* 0xC0,0x2B */
    &s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384,       /* 0xC0,0x2C */
    &s2n_ecdhe_ecdsa_with_chacha20_poly1305_sha256, /* 0xCC,0xA9 */
    &s2n_rsa_with_rc4_128_md5,                      /* 0x00,0x04 */
    &s2n_rsa_with_rc4_128_sha,                      /* 0x00,0x05 */
    &s2n_rsa_with_3des_ede_cbc_sha,                 /* 0x00,0x0A */
    &s2n_dhe_rsa_with_3des_ede_cbc_sha,             /* 0x00,0x16 */
    &s2n_rsa_with_aes_128_cbc_sha,                  /* 0x00,0x2F */
    &s2n_dhe_rsa_with_aes_128_cbc_sha,              /* 0x00,0x33 */
    &s2n_rsa_with_aes_256_cbc_sha,                  /* 0x00,0x35 */
    &s2n_dhe_rsa_with_aes_256_cbc_sha,              /* 0x00,0x39 */
    &s2n_rsa_with_aes_128_cbc_sha256,               /* 0x00,0x3C */
    &s2n_rsa_with_aes_256_cbc_sha256,               /* 0x00,0x3D */
    &s2n_dhe_rsa_with_aes_128_cbc_sha256,           /* 0x00,0x67 */
    &s2n_dhe_rsa_with_aes_256_cbc_sha256,           /* 0x00,0x6B */
    &s2n_rsa_with_aes_128_gcm_sha256,               /* 0x00,0x9C */
    &s2n_rsa_with_aes_256_gcm_sha384,               /* 0x00,0x9D */
    &s2n_dhe_rsa_with_aes_128_gcm_sha256,           /* 0x00,0x9E */
    &s2n_dhe_rsa_with_aes_256_gcm_sha384,           /* 0x00,0x9F */
    &s2n_ecdhe_rsa_with_rc4_128_sha,                /* 0xC0,0x11 */
    &s2n_ecdhe_rsa_with_3des_ede_cbc_sha,           /* 0xC0,0x12 */
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha,            /* 0xC0,0x13 */
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha,            /* 0xC0,0x14 */
    &s2n_ecdhe_rsa_with_aes_128_cbc_sha256,         /* 0xC0,0x27 */
    &s2n_ecdhe_rsa_with_aes_256_cbc_sha384,         /* 0xC0,0x28 */
    &s2n_ecdhe_rsa_with_aes_128_gcm_sha256,         /* 0xC0,0x2F */
    &s2n_ecdhe_rsa_with_aes_256_gcm_sha384,         /* 0xC0,0x30 */
    &s2n_ecdhe_rsa_with_chacha20_poly1305_sha256,   /* 0xCC,0xA8 */
    &s2n_dhe_rsa_with_chacha20_poly1305_sha256,     /* 0xCC,0xAA */
};

/* All cipher suites, but with ECDSA priority. Exposed for integration testing. */
const struct s2n_cipher_preferences cipher_preferences_test_ecdsa_priority = {
    .count = s2n_array_len(s2n_ecdsa_priority_cipher_suites),
    .suites = s2n_ecdsa_priority_cipher_suites,
};

static struct s2n_cipher_suite *s2n_all_tls13_cipher_suites[] = {
    &s2n_tls13_aes_128_gcm_sha256,       /* 0x13,0x01 */
    &s2n_tls13_aes_256_gcm_sha384,       /* 0x13,0x02 */
    &s2n_tls13_chacha20_poly1305_sha256, /* 0x13,0x03 */
};

const struct s2n_cipher_preferences cipher_preferences_test_all_tls13 = {
    .count = s2n_array_len(s2n_all_tls13_cipher_suites),
    .suites = s2n_all_tls13_cipher_suites,
};

static bool should_init_crypto = true;
static bool crypto_initialized = false;
int s2n_crypto_disable_init(void)
{
    POSIX_ENSURE(!crypto_initialized, S2N_ERR_INITIALIZED);
    should_init_crypto = false;
    return S2N_SUCCESS;
}

/* Determines cipher suite availability and selects record algorithms */
int s2n_cipher_suites_init(void)
{
    const int num_cipher_suites = s2n_array_len(s2n_all_cipher_suites);
    for (int i = 0; i < num_cipher_suites; i++) {
        struct s2n_cipher_suite *cur_suite = s2n_all_cipher_suites[i];
        cur_suite->available = 0;
        cur_suite->record_alg = NULL;

        /* Find the highest priority supported record algorithm */
        for (int j = 0; j < cur_suite->num_record_algs; j++) {
            /* Can we use the record algorithm's cipher? Won't be available if the system CPU architecture
             * doesn't support it or if the libcrypto lacks the feature. All hmac_algs are supported.
             */
            if (cur_suite->all_record_algs[j]->cipher->is_available()) {
                /* Found a supported record algorithm. Use it. */
                cur_suite->available = 1;
                cur_suite->record_alg = cur_suite->all_record_algs[j];
                break;
            }
        }

        /* Mark PQ cipher suites as unavailable if PQ is disabled */
        if (s2n_kex_includes(cur_suite->key_exchange_alg, &s2n_kem) && !s2n_pq_is_enabled()) {
            cur_suite->available = 0;
            cur_suite->record_alg = NULL;
        }

        /* Initialize SSLv3 cipher suite if SSLv3 utilizes a different record algorithm */
        if (cur_suite->sslv3_record_alg && cur_suite->sslv3_record_alg->cipher->is_available()) {
            struct s2n_blob cur_suite_mem = { 0 };
            POSIX_GUARD(s2n_blob_init(&cur_suite_mem, (uint8_t *) cur_suite, sizeof(struct s2n_cipher_suite)));
            struct s2n_blob new_suite_mem = { 0 };
            POSIX_GUARD(s2n_dup(&cur_suite_mem, &new_suite_mem));

            struct s2n_cipher_suite *new_suite = (struct s2n_cipher_suite *) (void *) new_suite_mem.data;
            new_suite->available = 1;
            new_suite->record_alg = cur_suite->sslv3_record_alg;
            cur_suite->sslv3_cipher_suite = new_suite;
        } else {
            cur_suite->sslv3_cipher_suite = cur_suite;
        }
    }

    if (should_init_crypto) {
#if !S2N_OPENSSL_VERSION_AT_LEAST(1, 1, 0)
        /*https://wiki.openssl.org/index.php/Manual:OpenSSL_add_all_algorithms(3)*/
        OpenSSL_add_all_algorithms();
#else
        OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CRYPTO_STRINGS | OPENSSL_INIT_ADD_ALL_CIPHERS | OPENSSL_INIT_ADD_ALL_DIGESTS, NULL);
#endif
    }

    crypto_initialized = true;

    return S2N_SUCCESS;
}

/* Reset any selected record algorithms */
S2N_RESULT s2n_cipher_suites_cleanup(void)
{
    const int num_cipher_suites = s2n_array_len(s2n_all_cipher_suites);
    for (int i = 0; i < num_cipher_suites; i++) {
        struct s2n_cipher_suite *cur_suite = s2n_all_cipher_suites[i];
        cur_suite->available = 0;
        cur_suite->record_alg = NULL;

        /* Release custom SSLv3 cipher suites */
        if (cur_suite->sslv3_cipher_suite != cur_suite) {
            RESULT_GUARD_POSIX(s2n_free_object((uint8_t **) &cur_suite->sslv3_cipher_suite, sizeof(struct s2n_cipher_suite)));
        }
        cur_suite->sslv3_cipher_suite = NULL;
    }

    if (should_init_crypto) {
#if !S2N_OPENSSL_VERSION_AT_LEAST(1, 1, 0)
        /*https://wiki.openssl.org/index.php/Manual:OpenSSL_add_all_algorithms(3)*/
        EVP_cleanup();

        /* per the reqs here https://www.openssl.org/docs/man1.1.0/crypto/OPENSSL_init_crypto.html we don't explicitly call
        * cleanup in later versions */
#endif
    }
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_cipher_suite_from_iana(const uint8_t *iana, size_t iana_len, struct s2n_cipher_suite **cipher_suite)
{
    RESULT_ENSURE_REF(cipher_suite);
    *cipher_suite = NULL;
    RESULT_ENSURE_REF(iana);
    RESULT_ENSURE_EQ(iana_len, S2N_TLS_CIPHER_SUITE_LEN);

    int low = 0;
    int top = s2n_array_len(s2n_all_cipher_suites) - 1;

    /* Perform a textbook binary search */
    while (low <= top) {
        /* Check in the middle */
        size_t mid = low + ((top - low) / 2);
        int m = memcmp(s2n_all_cipher_suites[mid]->iana_value, iana, S2N_TLS_CIPHER_SUITE_LEN);

        if (m == 0) {
            *cipher_suite = s2n_all_cipher_suites[mid];
            return S2N_RESULT_OK;
        } else if (m > 0) {
            top = mid - 1;
        } else if (m < 0) {
            low = mid + 1;
        }
    }
    RESULT_BAIL(S2N_ERR_CIPHER_NOT_SUPPORTED);
}

int s2n_set_cipher_as_client(struct s2n_connection *conn, uint8_t wire[S2N_TLS_CIPHER_SUITE_LEN])
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->secure);
    POSIX_ENSURE_REF(conn->secure->cipher_suite);

    const struct s2n_security_policy *security_policy = NULL;
    POSIX_GUARD(s2n_connection_get_security_policy(conn, &security_policy));
    POSIX_ENSURE_REF(security_policy);

    /**
     * Ensure that the wire cipher suite is contained in the security
     * policy, and thus was offered by the client.
     *
     *= https://www.rfc-editor.org/rfc/rfc8446#4.1.3
     *# A client which receives a
     *# cipher suite that was not offered MUST abort the handshake with an
     *# "illegal_parameter" alert.
     *
     *= https://www.rfc-editor.org/rfc/rfc8446#4.1.4
     *# A client which receives a cipher suite that was not offered MUST
     *# abort the handshake.
     *
     *= https://www.rfc-editor.org/rfc/rfc8446#4.1.4
     *# Upon receipt of a HelloRetryRequest, the client MUST check the
     *# legacy_version, legacy_session_id_echo, cipher_suite
     **/
    struct s2n_cipher_suite *cipher_suite = NULL;
    for (size_t i = 0; i < security_policy->cipher_preferences->count; i++) {
        const uint8_t *ours = security_policy->cipher_preferences->suites[i]->iana_value;
        if (s2n_constant_time_equals(wire, ours, S2N_TLS_CIPHER_SUITE_LEN)) {
            cipher_suite = security_policy->cipher_preferences->suites[i];
            break;
        }
    }
    POSIX_ENSURE(cipher_suite != NULL, S2N_ERR_CIPHER_NOT_SUPPORTED);

    POSIX_ENSURE(cipher_suite->available, S2N_ERR_CIPHER_NOT_SUPPORTED);

    /** Clients MUST verify
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.11
     *# that the server selected a cipher suite
     *# indicating a Hash associated with the PSK
     **/
    if (conn->psk_params.chosen_psk) {
        POSIX_ENSURE(cipher_suite->prf_alg == conn->psk_params.chosen_psk->hmac_alg,
                S2N_ERR_CIPHER_NOT_SUPPORTED);
    }

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#4.1.4
     *# Upon receiving
     *# the ServerHello, clients MUST check that the cipher suite supplied in
     *# the ServerHello is the same as that in the HelloRetryRequest and
     *# otherwise abort the handshake with an "illegal_parameter" alert.
     **/
    if (s2n_is_hello_retry_handshake(conn) && !s2n_is_hello_retry_message(conn)) {
        POSIX_ENSURE(conn->secure->cipher_suite->iana_value == cipher_suite->iana_value, S2N_ERR_CIPHER_NOT_SUPPORTED);
        return S2N_SUCCESS;
    }

    conn->secure->cipher_suite = cipher_suite;

    /* For SSLv3 use SSLv3-specific ciphers */
    if (conn->actual_protocol_version == S2N_SSLv3) {
        conn->secure->cipher_suite = conn->secure->cipher_suite->sslv3_cipher_suite;
        POSIX_ENSURE_REF(conn->secure->cipher_suite);
    }

    return 0;
}

static int s2n_wire_ciphers_contain(const uint8_t *match, const uint8_t *wire, uint32_t count, uint32_t cipher_suite_len)
{
    for (size_t i = 0; i < count; i++) {
        const uint8_t *theirs = wire + (i * cipher_suite_len) + (cipher_suite_len - S2N_TLS_CIPHER_SUITE_LEN);

        if (s2n_constant_time_equals(match, theirs, S2N_TLS_CIPHER_SUITE_LEN)) {
            return 1;
        }
    }

    return 0;
}

bool s2n_cipher_suite_uses_chacha20_alg(struct s2n_cipher_suite *cipher_suite)
{
    return cipher_suite && cipher_suite->record_alg && cipher_suite->record_alg->cipher == &s2n_chacha20_poly1305;
}

/* Iff the server has enabled allow_chacha20_boosting and the client has a chacha20 cipher suite as its most
 * preferred cipher suite, then we have mutual chacha20 boosting support.
 */
static S2N_RESULT s2n_validate_chacha20_boosting(const struct s2n_cipher_preferences *cipher_preferences, const uint8_t *wire,
        uint32_t cipher_suite_len)
{
    RESULT_ENSURE_REF(cipher_preferences);
    RESULT_ENSURE_REF(wire);

    RESULT_ENSURE_EQ(cipher_preferences->allow_chacha20_boosting, true);

    const uint8_t *clients_first_cipher_iana = wire + cipher_suite_len - S2N_TLS_CIPHER_SUITE_LEN;

    struct s2n_cipher_suite *client_first_cipher_suite = NULL;
    RESULT_GUARD(s2n_cipher_suite_from_iana(clients_first_cipher_iana, S2N_TLS_CIPHER_SUITE_LEN, &client_first_cipher_suite));
    RESULT_ENSURE_REF(client_first_cipher_suite);

    RESULT_ENSURE_EQ(s2n_cipher_suite_uses_chacha20_alg(client_first_cipher_suite), true);
    return S2N_RESULT_OK;
}

static int s2n_set_cipher_as_server(struct s2n_connection *conn, uint8_t *wire, uint32_t count, uint32_t cipher_suite_len)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->secure);

    uint8_t renegotiation_info_scsv[S2N_TLS_CIPHER_SUITE_LEN] = { TLS_EMPTY_RENEGOTIATION_INFO_SCSV };
    struct s2n_cipher_suite *higher_vers_match = NULL;
    struct s2n_cipher_suite *non_chacha20_match = NULL;

    /* RFC 7507 - If client is attempting to negotiate a TLS Version that is lower than the highest supported server
     * version, and the client cipher list contains TLS_FALLBACK_SCSV, then the server must abort the connection since
     * TLS_FALLBACK_SCSV should only be present when the client previously failed to negotiate a higher TLS version.
     */
    if (conn->client_protocol_version < conn->server_protocol_version) {
        uint8_t fallback_scsv[S2N_TLS_CIPHER_SUITE_LEN] = { TLS_FALLBACK_SCSV };
        if (s2n_wire_ciphers_contain(fallback_scsv, wire, count, cipher_suite_len)) {
            POSIX_BAIL(S2N_ERR_FALLBACK_DETECTED);
        }
    }

    if (s2n_wire_ciphers_contain(renegotiation_info_scsv, wire, count, cipher_suite_len)) {
        /** For renegotiation handshakes:
         *= https://www.rfc-editor.org/rfc/rfc5746#3.7
         *# o  When a ClientHello is received, the server MUST verify that it
         *#    does not contain the TLS_EMPTY_RENEGOTIATION_INFO_SCSV SCSV.  If
         *#    the SCSV is present, the server MUST abort the handshake.
         */
        POSIX_ENSURE(!s2n_handshake_is_renegotiation(conn), S2N_ERR_BAD_MESSAGE);

        /** For initial handshakes:
         *= https://www.rfc-editor.org/rfc/rfc5746#3.6
         *# o  When a ClientHello is received, the server MUST check if it
         *#    includes the TLS_EMPTY_RENEGOTIATION_INFO_SCSV SCSV.  If it does,
         *#    set the secure_renegotiation flag to TRUE.
         */
        conn->secure_renegotiation = 1;
    }

    const struct s2n_security_policy *security_policy = NULL;
    POSIX_GUARD(s2n_connection_get_security_policy(conn, &security_policy));

    const struct s2n_cipher_preferences *cipher_preferences = security_policy->cipher_preferences;
    POSIX_ENSURE_REF(cipher_preferences);

    bool try_chacha20_boosting = s2n_result_is_ok(s2n_validate_chacha20_boosting(cipher_preferences, wire, cipher_suite_len));

    /*
     * s2n only respects server preference order and chooses the server's
     * most preferred mutually supported cipher suite.
     *
     * If chacha20 boosting is enabled, we prefer chacha20 cipher suites over all
     * other cipher suites.
     *
     * If no mutually supported cipher suites are found, we choose one with a version
     * too high for the current connection (higher_vers_match).
     */
    for (size_t i = 0; i < cipher_preferences->count; i++) {
        const uint8_t *ours = cipher_preferences->suites[i]->iana_value;

        if (s2n_wire_ciphers_contain(ours, wire, count, cipher_suite_len)) {
            /* We have a match */
            struct s2n_cipher_suite *match = cipher_preferences->suites[i];

            /* Never use TLS1.3 ciphers on a pre-TLS1.3 connection, and vice versa */
            if ((conn->actual_protocol_version >= S2N_TLS13) != (match->minimum_required_tls_version >= S2N_TLS13)) {
                continue;
            }

            /* If connection is for SSLv3, use SSLv3 version of suites */
            if (conn->actual_protocol_version == S2N_SSLv3) {
                match = match->sslv3_cipher_suite;
            }

            /* Skip the suite if we don't have an available implementation */
            if (!match->available) {
                continue;
            }

            /* Make sure the cipher is valid for available certs */
            if (s2n_is_cipher_suite_valid_for_auth(conn, match) != S2N_SUCCESS) {
                continue;
            }

            /* If the kex is not supported continue to the next candidate */
            bool kex_supported = false;
            POSIX_GUARD_RESULT(s2n_kex_supported(match, conn, &kex_supported));
            if (!kex_supported) {
                continue;
            }
            /* If the kex is not configured correctly continue to the next candidate */
            if (s2n_result_is_error(s2n_configure_kex(match, conn))) {
                continue;
            }

            /**
             *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.11
             *# The server MUST ensure that it selects a compatible PSK
             *# (if any) and cipher suite.
             **/
            if (conn->psk_params.chosen_psk != NULL) {
                if (match->prf_alg != conn->psk_params.chosen_psk->hmac_alg) {
                    continue;
                }
            }

            /* Don't immediately choose a cipher the connection shouldn't be able to support */
            if (conn->actual_protocol_version < match->minimum_required_tls_version) {
                if (!higher_vers_match) {
                    higher_vers_match = match;
                }
                continue;
            }

            /* The server and client have chacha20 boosting support enabled AND the server identified a negotiable match */
            if (try_chacha20_boosting) {
                if (s2n_cipher_suite_uses_chacha20_alg(match)) {
                    conn->secure->cipher_suite = match;
                    return S2N_SUCCESS;
                }

                /* Save the valid non-chacha20 match in case no valid chacha20 match is found */
                if (!non_chacha20_match) {
                    non_chacha20_match = match;
                }
                continue;
            }

            conn->secure->cipher_suite = match;
            return S2N_SUCCESS;
        }
    }

    if (non_chacha20_match) {
        conn->secure->cipher_suite = non_chacha20_match;
        return S2N_SUCCESS;
    }

    /* Settle for a cipher with a higher required proto version, if it was set */
    if (higher_vers_match) {
        conn->secure->cipher_suite = higher_vers_match;
        return S2N_SUCCESS;
    }

    POSIX_BAIL(S2N_ERR_CIPHER_NOT_SUPPORTED);
}

int s2n_set_cipher_as_sslv2_server(struct s2n_connection *conn, uint8_t *wire, uint16_t count)
{
    return s2n_set_cipher_as_server(conn, wire, count, S2N_SSLv2_CIPHER_SUITE_LEN);
}

int s2n_set_cipher_as_tls_server(struct s2n_connection *conn, uint8_t *wire, uint16_t count)
{
    return s2n_set_cipher_as_server(conn, wire, count, S2N_TLS_CIPHER_SUITE_LEN);
}

bool s2n_cipher_suite_requires_ecc_extension(struct s2n_cipher_suite *cipher)
{
    if (!cipher) {
        return false;
    }

    /* TLS1.3 does not include key exchange algorithms in its cipher suites,
     * but the elliptic curves extension is always required. */
    if (cipher->minimum_required_tls_version >= S2N_TLS13) {
        return true;
    }

    if (s2n_kex_includes(cipher->key_exchange_alg, &s2n_ecdhe)) {
        return true;
    }

    return false;
}

bool s2n_cipher_suite_requires_pq_extension(struct s2n_cipher_suite *cipher)
{
    if (!cipher) {
        return false;
    }

    if (s2n_kex_includes(cipher->key_exchange_alg, &s2n_kem)) {
        return true;
    }
    return false;
}
