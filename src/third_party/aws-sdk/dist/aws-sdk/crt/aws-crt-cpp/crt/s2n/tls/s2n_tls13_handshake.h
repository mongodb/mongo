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

#include "crypto/s2n_tls13_keys.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_key_update.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_safety.h"

int s2n_tls13_mac_verify(struct s2n_tls13_keys *keys, struct s2n_blob *finished_verify, struct s2n_blob *wire_verify);

#define s2n_get_hash_state(hash_state, alg, conn) \
    struct s2n_hash_state hash_state = { 0 };     \
    POSIX_GUARD(s2n_handshake_get_hash_state(conn, alg, &hash_state));

/* Creates a reference to tls13_keys from connection */
#define s2n_tls13_connection_keys(keys, conn)                               \
    DEFER_CLEANUP(struct s2n_tls13_keys keys = { 0 }, s2n_tls13_keys_free); \
    POSIX_GUARD(s2n_tls13_keys_from_conn(&keys, conn));

int s2n_tls13_keys_from_conn(struct s2n_tls13_keys *keys, struct s2n_connection *conn);

int s2n_tls13_compute_shared_secret(struct s2n_connection *conn, struct s2n_blob *shared_secret);
int s2n_update_application_traffic_keys(struct s2n_connection *conn, s2n_mode mode, keyupdate_status status);

int s2n_server_hello_retry_recreate_transcript(struct s2n_connection *conn);
