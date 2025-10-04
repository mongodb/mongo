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

#include "stuffer/s2n_stuffer.h"
#include "utils/s2n_blob.h"

#define S2N_STATE_LIFETIME_IN_NANOS           54000000000000 /* 15 hours */
#define S2N_TLS12_STATE_SIZE_IN_BYTES         (1 + 8 + 1 + S2N_TLS_CIPHER_SUITE_LEN + S2N_TLS_SECRET_LEN + 1)
#define S2N_TLS13_FIXED_STATE_SIZE            21
#define S2N_TLS13_FIXED_EARLY_DATA_STATE_SIZE 3

/* This is used in session ticket validation. This controls how far in the future
 * the session ticket issue time can be while still being accepted.
 */
#define MAX_ALLOWED_CLOCK_SKEW_SEC     3600
#define S2N_TLS_SESSION_CACHE_TTL      (6 * 60 * 60)
#define S2N_TICKET_KEY_NAME_LEN        16
#define S2N_TICKET_AAD_IMPLICIT_LEN    12
#define S2N_TICKET_AAD_LEN             (S2N_TICKET_AAD_IMPLICIT_LEN + S2N_TICKET_KEY_NAME_LEN)
#define S2N_AES256_KEY_LEN             32
#define ONE_SEC_IN_NANOS               1000000000
#define ONE_MILLISEC_IN_NANOS          1000000
#define ONE_WEEK_IN_SEC                604800
#define S2N_TICKET_INFO_SIZE           32
#define S2N_TICKET_VERSION_SIZE        1
#define S2N_TLS12_TICKET_SIZE_IN_BYTES (S2N_TICKET_VERSION_SIZE + S2N_TICKET_KEY_NAME_LEN \
        + S2N_TICKET_INFO_SIZE + S2N_TLS_GCM_IV_LEN + S2N_TLS12_STATE_SIZE_IN_BYTES + S2N_TLS_GCM_TAG_LEN)

#define S2N_TICKET_ENCRYPT_DECRYPT_KEY_LIFETIME_IN_NANOS 7200000000000  /* 2 hours */
#define S2N_TICKET_DECRYPT_KEY_LIFETIME_IN_NANOS         46800000000000 /* 13 hours */
#define S2N_STATE_FORMAT_LEN                             1
#define S2N_TICKET_LIFETIME_HINT_LEN                     4
#define S2N_SESSION_TICKET_SIZE_LEN                      2
#define S2N_GREATER_OR_EQUAL                             1
#define S2N_LESS_THAN                                    -1

#define S2N_TLS12_SESSION_SIZE S2N_STATE_FORMAT_LEN + S2N_SESSION_TICKET_SIZE_LEN \
        + S2N_TLS12_TICKET_SIZE_IN_BYTES + S2N_TLS12_STATE_SIZE_IN_BYTES

struct s2n_connection;
struct s2n_config;

struct s2n_ticket_key {
    unsigned char key_name[S2N_TICKET_KEY_NAME_LEN];
    uint8_t aes_key[S2N_AES256_KEY_LEN];
    uint8_t implicit_aad[S2N_TICKET_AAD_IMPLICIT_LEN];
    uint64_t intro_timestamp;
};

struct s2n_ticket_key_weight {
    double key_weight;
    uint8_t key_index;
};

struct s2n_ticket_fields {
    struct s2n_blob session_secret;
    uint32_t ticket_age_add;
};

struct s2n_session_ticket {
    struct s2n_blob ticket_data;
    uint32_t session_lifetime;
};

struct s2n_ticket_key *s2n_find_ticket_key(struct s2n_config *config, const uint8_t name[S2N_TICKET_KEY_NAME_LEN]);
S2N_RESULT s2n_resume_encrypt_session_ticket(struct s2n_connection *conn, struct s2n_stuffer *to);
S2N_RESULT s2n_resume_decrypt_session_ticket(struct s2n_connection *conn, struct s2n_stuffer *from);
S2N_RESULT s2n_resume_decrypt_session_cache(struct s2n_connection *conn, struct s2n_stuffer *from);
S2N_RESULT s2n_config_is_encrypt_key_available(struct s2n_config *config);
int s2n_verify_unique_ticket_key(struct s2n_config *config, uint8_t *hash, uint16_t *insert_index);
int s2n_config_wipe_expired_ticket_crypto_keys(struct s2n_config *config, int8_t expired_key_index);
int s2n_config_store_ticket_key(struct s2n_config *config, struct s2n_ticket_key *key);

typedef enum {
    S2N_STATE_WITH_SESSION_ID = 0,
    S2N_STATE_WITH_SESSION_TICKET
} s2n_client_tls_session_state_format;

typedef enum {
    S2N_SERIALIZED_FORMAT_TLS12_V1 = 1,
    S2N_SERIALIZED_FORMAT_TLS13_V1,
    S2N_SERIALIZED_FORMAT_TLS12_V2,
    S2N_SERIALIZED_FORMAT_TLS12_V3,
} s2n_serial_format_version;

/* Used to specify the format of the ticket schema before encryption.
 *
 * This makes it easier to make changes to the ticket schema in the future
 * as it allows us to interpret and parse all ticket schemas.
 **/
typedef enum {
    S2N_PRE_ENCRYPTED_STATE_V1 = 1,
} s2n_pre_encrypted_state;

int s2n_allowed_to_cache_connection(struct s2n_connection *conn);
int s2n_resume_from_cache(struct s2n_connection *conn);
S2N_RESULT s2n_store_to_cache(struct s2n_connection *conn);
S2N_RESULT s2n_connection_get_session_state_size(struct s2n_connection *conn, size_t *state_size);
S2N_RESULT s2n_deserialize_resumption_state(struct s2n_connection *conn, struct s2n_blob *psk_identity,
        struct s2n_stuffer *from);
