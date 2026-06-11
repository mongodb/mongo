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

#include "tls/s2n_crypto_constants.h"
#include "tls/s2n_quic_support.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_result.h"

/* tls/s2n_tls13_secrets should eventually replace crypto/s2n_tls13_keys.h */
#include "crypto/s2n_tls13_keys.h"

typedef enum {
    S2N_NONE_SECRET = 0,
    S2N_EARLY_SECRET,
    S2N_HANDSHAKE_SECRET,
    S2N_MASTER_SECRET
} s2n_extract_secret_type_t;

struct s2n_tls13_secrets {
    uint8_t extract_secret[S2N_TLS13_SECRET_MAX_LEN];

    uint8_t client_early_secret[S2N_TLS13_SECRET_MAX_LEN];
    uint8_t client_handshake_secret[S2N_TLS13_SECRET_MAX_LEN];
    uint8_t server_handshake_secret[S2N_TLS13_SECRET_MAX_LEN];

    uint8_t client_app_secret[S2N_TLS13_SECRET_MAX_LEN];
    uint8_t server_app_secret[S2N_TLS13_SECRET_MAX_LEN];
    uint8_t resumption_master_secret[S2N_TLS13_SECRET_MAX_LEN];
    uint8_t exporter_master_secret[S2N_TLS13_SECRET_MAX_LEN];

    s2n_extract_secret_type_t extract_secret_type;
};

S2N_RESULT s2n_tls13_empty_transcripts_init();

S2N_RESULT s2n_tls13_secrets_update(struct s2n_connection *conn);
S2N_RESULT s2n_tls13_secrets_get(struct s2n_connection *conn, s2n_extract_secret_type_t secret_type,
        s2n_mode mode, struct s2n_blob *secret);
S2N_RESULT s2n_tls13_secrets_clean(struct s2n_connection *conn);

S2N_RESULT s2n_derive_binder_key(struct s2n_psk *psk, struct s2n_blob *output);
S2N_RESULT s2n_derive_resumption_master_secret(struct s2n_connection *conn);
S2N_RESULT s2n_derive_exporter_master_secret(struct s2n_connection *conn, struct s2n_blob *output);
