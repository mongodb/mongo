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

#include "api/s2n.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_tls_parameters.h"
#include "utils/s2n_result.h"

struct s2n_connection;

struct s2n_post_handshake {
    struct s2n_stuffer in;
    uint8_t header_in[TLS_HANDSHAKE_HEADER_LENGTH];
};

S2N_RESULT s2n_post_handshake_recv(struct s2n_connection *conn);
int s2n_post_handshake_send(struct s2n_connection *conn, s2n_blocked_status *blocked);
S2N_RESULT s2n_post_handshake_write_records(struct s2n_connection *conn, s2n_blocked_status *blocked);
S2N_RESULT s2n_post_handshake_process(struct s2n_connection *conn, struct s2n_stuffer *in, uint8_t message_type);
