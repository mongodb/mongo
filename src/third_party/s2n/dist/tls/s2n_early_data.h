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
#include "tls/s2n_crypto_constants.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_result.h"

struct s2n_psk;

typedef enum {
    S2N_UNKNOWN_EARLY_DATA_STATE = 0,
    S2N_EARLY_DATA_REQUESTED,
    S2N_EARLY_DATA_NOT_REQUESTED,
    S2N_EARLY_DATA_ACCEPTED,
    S2N_EARLY_DATA_REJECTED,
    S2N_END_OF_EARLY_DATA,
    S2N_EARLY_DATA_STATES_COUNT
} s2n_early_data_state;

S2N_RESULT s2n_connection_set_early_data_state(struct s2n_connection *conn, s2n_early_data_state state);

struct s2n_early_data_config {
    uint32_t max_early_data_size;
    uint8_t protocol_version;
    struct s2n_cipher_suite *cipher_suite;
    struct s2n_blob application_protocol;
    struct s2n_blob context;
};
S2N_CLEANUP_RESULT s2n_early_data_config_free(struct s2n_early_data_config *config);
S2N_RESULT s2n_early_data_config_clone(struct s2n_psk *new_psk, struct s2n_early_data_config *old_config);

struct s2n_offered_early_data {
    struct s2n_connection *conn;
};

bool s2n_early_data_is_valid_for_connection(struct s2n_connection *conn);
S2N_RESULT s2n_early_data_accept_or_reject(struct s2n_connection *conn);

S2N_RESULT s2n_early_data_get_server_max_size(struct s2n_connection *conn, uint32_t *max_early_data_size);

S2N_RESULT s2n_early_data_record_bytes(struct s2n_connection *conn, ssize_t data_len);
S2N_RESULT s2n_early_data_validate_send(struct s2n_connection *conn, uint32_t bytes_to_send);
S2N_RESULT s2n_early_data_validate_recv(struct s2n_connection *conn);
bool s2n_early_data_is_trial_decryption_allowed(struct s2n_connection *conn, uint8_t record_type);

int s2n_connection_set_early_data_expected(struct s2n_connection *conn);
int s2n_connection_set_end_of_early_data(struct s2n_connection *conn);
