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

#include <stdint.h>

#include "crypto/s2n_certificate.h"
#include "crypto/s2n_cipher.h"
#include "crypto/s2n_hmac.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_crypto.h"
#include "tls/s2n_kem_preferences.h"
#include "tls/s2n_tls_parameters.h"

/* Key exchange flags that can be OR'ed */
#define S2N_KEY_EXCHANGE_DH  0x01 /* Diffie-Hellman key exchange, including ephemeral */
#define S2N_KEY_EXCHANGE_EPH 0x02 /* Ephemeral key exchange */
#define S2N_KEY_EXCHANGE_ECC 0x04 /* Elliptic curve cryptography */

#define S2N_MAX_POSSIBLE_RECORD_ALGS 2

/* Kept up-to-date by s2n_cipher_suite_test */
#define S2N_CIPHER_SUITE_COUNT 37

/* Record algorithm flags that can be OR'ed */
#define S2N_TLS12_AES_GCM_AEAD_NONCE     0x01
#define S2N_TLS12_CHACHA_POLY_AEAD_NONCE 0x02
#define S2N_TLS13_RECORD_AEAD_NONCE      0x04

/* From RFC: https://tools.ietf.org/html/rfc8446#section-5.5
 * For AES-GCM, up to 2^24.5 full-size records (about 24 million) may be
 * encrypted on a given connection while keeping a safety margin of
 * approximately 2^-57 for Authenticated Encryption (AE) security.
 * S2N_TLS13_MAXIMUM_RECORD_NUMBER is 2^24.5 rounded down to the nearest whole number
 * minus 1 for the key update message.
 */
#define S2N_TLS13_AES_GCM_MAXIMUM_RECORD_NUMBER ((uint64_t) 23726565)

typedef enum {
    S2N_AUTHENTICATION_RSA = 0,
    S2N_AUTHENTICATION_ECDSA,
    S2N_AUTHENTICATION_METHOD_SENTINEL
} s2n_authentication_method;

/* Used by TLS 1.3 CipherSuites (Eg TLS_AES_128_GCM_SHA256 "0x1301") where the Auth method will be specified by the
 * SignatureScheme Extension, not the CipherSuite. */
#define S2N_AUTHENTICATION_METHOD_TLS13 S2N_AUTHENTICATION_METHOD_SENTINEL

struct s2n_record_algorithm {
    const struct s2n_cipher *cipher;
    s2n_hmac_algorithm hmac_alg;
    uint32_t flags;
    uint64_t encryption_limit;
};

/* Verbose names to avoid confusion with s2n_cipher. Exposed for unit tests */
extern const struct s2n_record_algorithm s2n_record_alg_null;
extern const struct s2n_record_algorithm s2n_record_alg_rc4_md5;
extern const struct s2n_record_algorithm s2n_record_alg_rc4_sha;
extern const struct s2n_record_algorithm s2n_record_alg_3des_sha;
extern const struct s2n_record_algorithm s2n_record_alg_aes128_sha;
extern const struct s2n_record_algorithm s2n_record_alg_aes128_sha_composite;
extern const struct s2n_record_algorithm s2n_record_alg_aes128_sha256;
extern const struct s2n_record_algorithm s2n_record_alg_aes128_sha256_composite;
extern const struct s2n_record_algorithm s2n_record_alg_aes256_sha;
extern const struct s2n_record_algorithm s2n_record_alg_aes256_sha_composite;
extern const struct s2n_record_algorithm s2n_record_alg_aes256_sha256;
extern const struct s2n_record_algorithm s2n_record_alg_aes256_sha256_composite;
extern const struct s2n_record_algorithm s2n_record_alg_aes256_sha384;
extern const struct s2n_record_algorithm s2n_record_alg_aes128_gcm;
extern const struct s2n_record_algorithm s2n_record_alg_aes256_gcm;
extern const struct s2n_record_algorithm s2n_record_alg_chacha20_poly1305;
extern const struct s2n_record_algorithm s2n_tls13_record_alg_aes128_gcm;
extern const struct s2n_record_algorithm s2n_tls13_record_alg_chacha20_poly1305;

struct s2n_cipher_suite {
    /* Is there an implementation available? Set in s2n_cipher_suites_init() */
    unsigned int available : 1;

    /* Cipher name in Openssl format */
    const char *name;

    /* Cipher name in IANA format */
    const char *iana_name;

    const uint8_t iana_value[S2N_TLS_CIPHER_SUITE_LEN];

    const struct s2n_kex *key_exchange_alg;

    const s2n_authentication_method auth_method;

    /* Algorithms used for per-record security. Set in s2n_cipher_suites_init() */
    const struct s2n_record_algorithm *record_alg;

    /* List of all possible record alg implementations in descending priority */
    const struct s2n_record_algorithm *all_record_algs[S2N_MAX_POSSIBLE_RECORD_ALGS];
    const uint8_t num_record_algs;

    /* SSLv3 utilizes HMAC differently from TLS */
    const struct s2n_record_algorithm *sslv3_record_alg;
    struct s2n_cipher_suite *sslv3_cipher_suite;

    /* RFC 5426(TLS1.2) allows cipher suite defined PRFs. Cipher suites defined in and before TLS1.2 will use
     * P_hash with SHA256 when TLS1.2 is negotiated.
     */
    const s2n_hmac_algorithm prf_alg;

    const uint8_t minimum_required_tls_version;
};

/* Never negotiated */
extern struct s2n_cipher_suite s2n_null_cipher_suite;

extern struct s2n_cipher_suite s2n_rsa_with_rc4_128_md5;
extern struct s2n_cipher_suite s2n_rsa_with_rc4_128_sha;
extern struct s2n_cipher_suite s2n_rsa_with_3des_ede_cbc_sha;
extern struct s2n_cipher_suite s2n_dhe_rsa_with_3des_ede_cbc_sha;
extern struct s2n_cipher_suite s2n_rsa_with_aes_128_cbc_sha;
extern struct s2n_cipher_suite s2n_dhe_rsa_with_aes_128_cbc_sha;
extern struct s2n_cipher_suite s2n_rsa_with_aes_256_cbc_sha;
extern struct s2n_cipher_suite s2n_dhe_rsa_with_aes_256_cbc_sha;
extern struct s2n_cipher_suite s2n_rsa_with_aes_128_cbc_sha256;
extern struct s2n_cipher_suite s2n_rsa_with_aes_256_cbc_sha256;
extern struct s2n_cipher_suite s2n_dhe_rsa_with_aes_128_cbc_sha256;
extern struct s2n_cipher_suite s2n_dhe_rsa_with_aes_256_cbc_sha256;
extern struct s2n_cipher_suite s2n_rsa_with_aes_128_gcm_sha256;
extern struct s2n_cipher_suite s2n_rsa_with_aes_256_gcm_sha384;
extern struct s2n_cipher_suite s2n_dhe_rsa_with_aes_128_gcm_sha256;
extern struct s2n_cipher_suite s2n_dhe_rsa_with_aes_256_gcm_sha384;
extern struct s2n_cipher_suite s2n_ecdhe_ecdsa_with_aes_128_cbc_sha;
extern struct s2n_cipher_suite s2n_ecdhe_ecdsa_with_aes_256_cbc_sha;
extern struct s2n_cipher_suite s2n_ecdhe_rsa_with_3des_ede_cbc_sha;
extern struct s2n_cipher_suite s2n_ecdhe_rsa_with_aes_128_cbc_sha;
extern struct s2n_cipher_suite s2n_ecdhe_rsa_with_aes_256_cbc_sha;
extern struct s2n_cipher_suite s2n_ecdhe_ecdsa_with_aes_128_cbc_sha256;
extern struct s2n_cipher_suite s2n_ecdhe_ecdsa_with_aes_256_cbc_sha384;
extern struct s2n_cipher_suite s2n_ecdhe_rsa_with_aes_128_cbc_sha256;
extern struct s2n_cipher_suite s2n_ecdhe_rsa_with_aes_256_cbc_sha384;
extern struct s2n_cipher_suite s2n_ecdhe_ecdsa_with_aes_128_gcm_sha256;
extern struct s2n_cipher_suite s2n_ecdhe_ecdsa_with_aes_256_gcm_sha384;
extern struct s2n_cipher_suite s2n_ecdhe_rsa_with_aes_128_gcm_sha256;
extern struct s2n_cipher_suite s2n_ecdhe_rsa_with_aes_256_gcm_sha384;
extern struct s2n_cipher_suite s2n_ecdhe_rsa_with_chacha20_poly1305_sha256;
extern struct s2n_cipher_suite s2n_dhe_rsa_with_chacha20_poly1305_sha256;
extern struct s2n_cipher_suite s2n_ecdhe_ecdsa_with_chacha20_poly1305_sha256;
extern struct s2n_cipher_suite s2n_ecdhe_rsa_with_rc4_128_sha;
extern struct s2n_cipher_suite s2n_ecdhe_kyber_rsa_with_aes_256_gcm_sha384;
extern struct s2n_cipher_suite s2n_tls13_aes_256_gcm_sha384;
extern struct s2n_cipher_suite s2n_tls13_aes_128_gcm_sha256;
extern struct s2n_cipher_suite s2n_tls13_chacha20_poly1305_sha256;

int s2n_cipher_suites_init(void);
S2N_RESULT s2n_cipher_suites_cleanup(void);
S2N_RESULT s2n_cipher_suite_from_iana(const uint8_t *iana, size_t iana_len, struct s2n_cipher_suite **cipher_suite);
bool s2n_cipher_suite_uses_chacha20_alg(struct s2n_cipher_suite *cipher_suite);
int s2n_set_cipher_as_client(struct s2n_connection *conn, uint8_t wire[S2N_TLS_CIPHER_SUITE_LEN]);
int s2n_set_cipher_as_sslv2_server(struct s2n_connection *conn, uint8_t *wire, uint16_t count);
int s2n_set_cipher_as_tls_server(struct s2n_connection *conn, uint8_t *wire, uint16_t count);
bool s2n_cipher_suite_requires_ecc_extension(struct s2n_cipher_suite *cipher);
bool s2n_cipher_suite_requires_pq_extension(struct s2n_cipher_suite *cipher);
