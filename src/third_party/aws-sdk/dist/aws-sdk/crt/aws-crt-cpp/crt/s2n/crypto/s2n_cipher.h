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

#include <openssl/aes.h>
#include <openssl/des.h>
#include <openssl/dh.h>
#include <openssl/evp.h>
#include <openssl/rc4.h>
#include <openssl/rsa.h>

#include "crypto/s2n_crypto.h"
#include "crypto/s2n_ktls_crypto.h"
#include "utils/s2n_blob.h"

#if defined(OPENSSL_IS_BORINGSSL) || defined(OPENSSL_IS_AWSLC)
    #define S2N_CIPHER_AEAD_API_AVAILABLE
#endif

struct s2n_session_key {
    EVP_CIPHER_CTX *evp_cipher_ctx;
#if defined(S2N_CIPHER_AEAD_API_AVAILABLE)
    EVP_AEAD_CTX *evp_aead_ctx;
#endif
};

struct s2n_stream_cipher {
    int (*decrypt)(struct s2n_session_key *key, struct s2n_blob *in, struct s2n_blob *out);
    int (*encrypt)(struct s2n_session_key *key, struct s2n_blob *in, struct s2n_blob *out);
};

struct s2n_cbc_cipher {
    uint8_t block_size;
    uint8_t record_iv_size;
    int (*decrypt)(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *in, struct s2n_blob *out);
    int (*encrypt)(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *in, struct s2n_blob *out);
};

struct s2n_aead_cipher {
    uint8_t fixed_iv_size;
    uint8_t record_iv_size;
    uint8_t tag_size;
    int (*decrypt)(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *add, struct s2n_blob *in, struct s2n_blob *out);
    int (*encrypt)(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *add, struct s2n_blob *in, struct s2n_blob *out);
};

struct s2n_composite_cipher {
    uint8_t block_size;
    uint8_t record_iv_size;
    uint8_t mac_key_size;
    int (*decrypt)(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *in, struct s2n_blob *out);
    int (*encrypt)(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *in, struct s2n_blob *out);
    int (*set_mac_write_key)(struct s2n_session_key *key, uint8_t *mac_key, uint32_t mac_size);
    int (*initial_hmac)(struct s2n_session_key *key, uint8_t *sequence_number, uint8_t content_type, uint16_t protocol_version,
            uint16_t payload_and_eiv_len, int *extra);
};

struct s2n_cipher {
    enum {
        S2N_STREAM,
        S2N_CBC,
        S2N_AEAD,
        S2N_COMPOSITE
    } type;
    union {
        struct s2n_stream_cipher stream;
        struct s2n_aead_cipher aead;
        struct s2n_cbc_cipher cbc;
        struct s2n_composite_cipher comp;
    } io;
    uint8_t key_material_size;
    bool (*is_available)(void);
    S2N_RESULT (*init)(struct s2n_session_key *key);
    S2N_RESULT (*set_decryption_key)(struct s2n_session_key *key, struct s2n_blob *in);
    S2N_RESULT (*set_encryption_key)(struct s2n_session_key *key, struct s2n_blob *in);
    S2N_RESULT (*destroy_key)(struct s2n_session_key *key);
    S2N_RESULT (*set_ktls_info)(struct s2n_ktls_crypto_info_inputs *inputs,
            struct s2n_ktls_crypto_info *crypto_info);
};

int s2n_session_key_alloc(struct s2n_session_key *key);
int s2n_session_key_free(struct s2n_session_key *key);

extern const struct s2n_cipher s2n_null_cipher;
extern const struct s2n_cipher s2n_rc4;
extern const struct s2n_cipher s2n_aes128;
extern const struct s2n_cipher s2n_aes256;
extern const struct s2n_cipher s2n_3des;
extern const struct s2n_cipher s2n_aes128_gcm;
extern const struct s2n_cipher s2n_aes256_gcm;
extern const struct s2n_cipher s2n_aes128_sha;
extern const struct s2n_cipher s2n_aes256_sha;
extern const struct s2n_cipher s2n_aes128_sha256;
extern const struct s2n_cipher s2n_aes256_sha256;
extern const struct s2n_cipher s2n_chacha20_poly1305;

extern const struct s2n_cipher s2n_tls13_aes128_gcm;
extern const struct s2n_cipher s2n_tls13_aes256_gcm;

S2N_RESULT s2n_rc4_init();
S2N_RESULT s2n_rc4_cleanup();
