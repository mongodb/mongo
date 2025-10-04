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

#include "crypto/s2n_certificate.h"
#include "crypto/s2n_cipher.h"
#include "crypto/s2n_dhe.h"
#include "crypto/s2n_ecc_evp.h"
#include "crypto/s2n_hash.h"
#include "crypto/s2n_hmac.h"
#include "crypto/s2n_pkey.h"
#include "crypto/s2n_signature.h"
#include "crypto/s2n_tls13_keys.h"
#include "tls/s2n_crypto_constants.h"
#include "tls/s2n_kem.h"
#include "tls/s2n_signature_scheme.h"
#include "tls/s2n_tls13_secrets.h"

struct s2n_kex_parameters {
    struct s2n_dh_params server_dh_params;
    struct s2n_ecc_evp_params server_ecc_evp_params;
    const struct s2n_ecc_named_curve *mutually_supported_curves[S2N_ECC_EVP_SUPPORTED_CURVES_COUNT];
    struct s2n_ecc_evp_params client_ecc_evp_params;
    struct s2n_kem_group_params server_kem_group_params;
    struct s2n_kem_group_params client_kem_group_params;
    const struct s2n_kem_group *mutually_supported_kem_groups[S2N_KEM_GROUPS_COUNT];
    struct s2n_kem_params kem_params;
    struct s2n_blob client_key_exchange_message;
    struct s2n_blob client_pq_kem_extension;
};

struct s2n_tls12_secrets {
    uint8_t rsa_premaster_secret[S2N_TLS_SECRET_LEN];
    uint8_t master_secret[S2N_TLS_SECRET_LEN];
};

struct s2n_secrets {
    union {
        struct s2n_tls12_secrets tls12;
        struct s2n_tls13_secrets tls13;
    } version;
    s2n_extract_secret_type_t extract_secret_type;
};

struct s2n_crypto_parameters {
    struct s2n_cipher_suite *cipher_suite;
    struct s2n_session_key client_key;
    struct s2n_session_key server_key;
    struct s2n_hmac_state client_record_mac;
    struct s2n_hmac_state server_record_mac;
    uint8_t client_implicit_iv[S2N_TLS_MAX_IV_LEN];
    uint8_t server_implicit_iv[S2N_TLS_MAX_IV_LEN];
    uint8_t client_sequence_number[S2N_TLS_SEQUENCE_NUM_LEN];
    uint8_t server_sequence_number[S2N_TLS_SEQUENCE_NUM_LEN];
};

S2N_RESULT s2n_crypto_parameters_new(struct s2n_crypto_parameters **params);
S2N_RESULT s2n_crypto_parameters_wipe(struct s2n_crypto_parameters *params);
S2N_CLEANUP_RESULT s2n_crypto_parameters_free(struct s2n_crypto_parameters **params);
S2N_RESULT s2n_crypto_parameters_switch(struct s2n_connection *conn);
