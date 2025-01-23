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

#include "api/s2n.h"
#include "crypto/s2n_hmac.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_early_data.h"
#include "utils/s2n_array.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_result.h"

typedef enum {
    S2N_PSK_TYPE_RESUMPTION = 0,
    S2N_PSK_TYPE_EXTERNAL,
} s2n_psk_type;

typedef enum {
    S2N_PSK_KE_UNKNOWN = 0,
    S2N_PSK_KE,
    S2N_PSK_DHE_KE,
} s2n_psk_key_exchange_mode;

struct s2n_psk {
    s2n_psk_type type;
    struct s2n_blob identity;
    struct s2n_blob secret;
    s2n_hmac_algorithm hmac_alg;
    uint32_t ticket_age_add;
    uint64_t ticket_issue_time;
    struct s2n_blob early_secret;
    struct s2n_early_data_config early_data_config;

    /* This field is used with session tickets to track the lifetime
     * of the original full handshake across multiple tickets.
     * See https://tools.ietf.org/rfc/rfc8446#section-4.6.1
     */
    uint64_t keying_material_expiration;
};
S2N_RESULT s2n_psk_init(struct s2n_psk *psk, s2n_psk_type type);
S2N_CLEANUP_RESULT s2n_psk_wipe(struct s2n_psk *psk);
S2N_RESULT s2n_psk_clone(struct s2n_psk *new_psk, struct s2n_psk *original_psk);

struct s2n_psk_parameters {
    s2n_psk_type type;
    struct s2n_array psk_list;
    uint16_t binder_list_size;
    uint16_t chosen_psk_wire_index;
    struct s2n_psk *chosen_psk;
    s2n_psk_key_exchange_mode psk_ke_mode;
};
S2N_RESULT s2n_psk_parameters_init(struct s2n_psk_parameters *params);
S2N_RESULT s2n_psk_parameters_offered_psks_size(struct s2n_psk_parameters *params, uint32_t *size);
S2N_CLEANUP_RESULT s2n_psk_parameters_wipe(struct s2n_psk_parameters *params);
S2N_CLEANUP_RESULT s2n_psk_parameters_wipe_secrets(struct s2n_psk_parameters *params);

struct s2n_offered_psk {
    struct s2n_blob identity;
    uint16_t wire_index;
    uint32_t obfuscated_ticket_age;
};

struct s2n_offered_psk_list {
    struct s2n_connection *conn;
    struct s2n_stuffer wire_data;
    uint16_t wire_index;
};

S2N_RESULT s2n_finish_psk_extension(struct s2n_connection *conn);

int s2n_psk_calculate_binder_hash(struct s2n_connection *conn, s2n_hmac_algorithm hmac_alg,
        const struct s2n_blob *partial_client_hello, struct s2n_blob *output_binder_hash);
int s2n_psk_calculate_binder(struct s2n_psk *psk, const struct s2n_blob *binder_hash,
        struct s2n_blob *output_binder);
int s2n_psk_verify_binder(struct s2n_connection *conn, struct s2n_psk *psk,
        const struct s2n_blob *partial_client_hello, struct s2n_blob *binder_to_verify);

S2N_RESULT s2n_connection_set_psk_type(struct s2n_connection *conn, s2n_psk_type type);
S2N_RESULT s2n_psk_validate_keying_material(struct s2n_connection *conn);
