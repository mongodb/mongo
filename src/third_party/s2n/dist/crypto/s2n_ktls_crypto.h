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

#include "utils/s2n_blob.h"

/* clang-format off */
#if defined(S2N_KTLS_SUPPORTED)
    #include <linux/tls.h>

    typedef struct tls12_crypto_info_aes_gcm_128 s2n_ktls_crypto_info_tls12_aes_gcm_128;
    typedef struct tls12_crypto_info_aes_gcm_256 s2n_ktls_crypto_info_tls12_aes_gcm_256;
#else
    #define TLS_1_2_VERSION 0
    #define TLS_1_3_VERSION 0

    #define TLS_CIPHER_AES_GCM_128 0
    typedef struct s2n_ktls_crypto_info_stub s2n_ktls_crypto_info_tls12_aes_gcm_128;
    #define TLS_CIPHER_AES_GCM_256 0
    typedef struct s2n_ktls_crypto_info_stub s2n_ktls_crypto_info_tls12_aes_gcm_256;
#endif
/* clang-format on */

/* To avoid compile-time errors, this must contain every field that we reference
 * from any crypto_info. However, it is only a placeholder-- it should never
 * actually be used.
 */
struct s2n_ktls_crypto_info_stub {
    struct {
        uint8_t version;
        uint8_t cipher_type;
    } info;
    uint8_t iv[1];
    uint8_t key[1];
    uint8_t salt[1];
    uint8_t rec_seq[1];
};

struct s2n_ktls_crypto_info {
    struct s2n_blob value;
    union {
        s2n_ktls_crypto_info_tls12_aes_gcm_128 aes_gcm_128;
        s2n_ktls_crypto_info_tls12_aes_gcm_256 aes_gcm_256;
    } ciphers;
};

struct s2n_ktls_crypto_info_inputs {
    struct s2n_blob iv;
    struct s2n_blob key;
    struct s2n_blob seq;
};
