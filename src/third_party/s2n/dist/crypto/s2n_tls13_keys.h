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

#include "crypto/s2n_hkdf.h"
#include "crypto/s2n_hmac.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_psk.h"
#include "tls/s2n_tls_parameters.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_safety.h"

/* Unlike TLS1.2 secrets, TLS1.3 secret lengths vary depending
 * on the hash algorithm used to calculate them.
 * We allocate enough space for the largest possible secret.
 * At the moment, that is 48 bytes for S2N_HASH_SHA384 and
 * matches the TLS1.2 secret length.
 */
#define S2N_TLS13_SECRET_MAX_LEN SHA384_DIGEST_LENGTH

struct s2n_tls13_keys {
    s2n_hmac_algorithm hmac_algorithm;
    s2n_hash_algorithm hash_algorithm;

    uint8_t size;

    /* we only need to keep these 2 rolling secrets at any point,
     * since other secrets to be used can be generated from these
     */
    struct s2n_blob extract_secret;
    struct s2n_blob derive_secret;
    uint8_t extract_secret_bytes[S2N_TLS13_SECRET_MAX_LEN];
    uint8_t derive_secret_bytes[S2N_TLS13_SECRET_MAX_LEN];

    struct s2n_hmac_state hmac;
};

/* Defines TLS 1.3 HKDF Labels */
extern const struct s2n_blob s2n_tls13_label_derived_secret;
extern const struct s2n_blob s2n_tls13_label_external_psk_binder_key;
extern const struct s2n_blob s2n_tls13_label_resumption_psk_binder_key;

extern const struct s2n_blob s2n_tls13_label_client_early_traffic_secret;
extern const struct s2n_blob s2n_tls13_label_early_exporter_master_secret;

extern const struct s2n_blob s2n_tls13_label_client_handshake_traffic_secret;
extern const struct s2n_blob s2n_tls13_label_server_handshake_traffic_secret;

extern const struct s2n_blob s2n_tls13_label_client_application_traffic_secret;
extern const struct s2n_blob s2n_tls13_label_server_application_traffic_secret;

extern const struct s2n_blob s2n_tls13_label_exporter_master_secret;
extern const struct s2n_blob s2n_tls13_label_resumption_master_secret;

extern const struct s2n_blob s2n_tls13_label_finished;

extern const struct s2n_blob s2n_tls13_label_exporter;

/* Traffic secret labels */

extern const struct s2n_blob s2n_tls13_label_traffic_secret_key;
extern const struct s2n_blob s2n_tls13_label_traffic_secret_iv;

#define s2n_tls13_key_blob(name, bytes) \
    s2n_stack_blob(name, bytes, S2N_TLS13_SECRET_MAX_LEN)

int s2n_tls13_keys_init(struct s2n_tls13_keys *handshake, s2n_hmac_algorithm alg);
int s2n_tls13_keys_free(struct s2n_tls13_keys *keys);

int s2n_tls13_derive_traffic_keys(struct s2n_tls13_keys *handshake, struct s2n_blob *secret, struct s2n_blob *key, struct s2n_blob *iv);
int s2n_tls13_derive_finished_key(struct s2n_tls13_keys *keys, struct s2n_blob *secret_key, struct s2n_blob *output_finish_key);
int s2n_tls13_calculate_finished_mac(struct s2n_tls13_keys *keys, struct s2n_blob *finished_key, struct s2n_hash_state *hash_state, struct s2n_blob *finished_verify);
int s2n_tls13_update_application_traffic_secret(struct s2n_tls13_keys *keys, struct s2n_blob *old_secret, struct s2n_blob *new_secret);
S2N_RESULT s2n_tls13_derive_session_ticket_secret(struct s2n_tls13_keys *keys, struct s2n_blob *resumption_secret,
        struct s2n_blob *ticket_nonce, struct s2n_blob *secret_blob);
