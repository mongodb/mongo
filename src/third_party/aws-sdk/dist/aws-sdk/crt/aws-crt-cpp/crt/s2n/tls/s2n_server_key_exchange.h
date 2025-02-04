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

#include "tls/s2n_connection.h"
#include "tls/s2n_kex.h"
#include "utils/s2n_blob.h"

int s2n_dhe_server_key_recv_read_data(struct s2n_connection *conn, struct s2n_blob *data_to_verify,
        struct s2n_kex_raw_server_data *raw_server_data);
int s2n_ecdhe_server_key_recv_read_data(struct s2n_connection *conn, struct s2n_blob *data_to_verify,
        struct s2n_kex_raw_server_data *raw_server_data);
int s2n_kem_server_key_recv_read_data(struct s2n_connection *conn, struct s2n_blob *data_to_verify,
        struct s2n_kex_raw_server_data *raw_server_data);
int s2n_hybrid_server_key_recv_read_data(struct s2n_connection *conn, struct s2n_blob *total_data_to_verify,
        struct s2n_kex_raw_server_data *raw_server_data);

int s2n_dhe_server_key_recv_parse_data(struct s2n_connection *conn, struct s2n_kex_raw_server_data *raw_server_data);
int s2n_ecdhe_server_key_recv_parse_data(struct s2n_connection *conn, struct s2n_kex_raw_server_data *raw_server_data);
int s2n_kem_server_key_recv_parse_data(struct s2n_connection *conn, struct s2n_kex_raw_server_data *raw_server_data);
int s2n_hybrid_server_key_recv_parse_data(struct s2n_connection *conn, struct s2n_kex_raw_server_data *raw_server_data);

int s2n_dhe_server_key_send(struct s2n_connection *conn, struct s2n_blob *data_to_sign);
int s2n_ecdhe_server_key_send(struct s2n_connection *conn, struct s2n_blob *data_to_sign);
int s2n_kem_server_key_send(struct s2n_connection *conn, struct s2n_blob *data_to_sign);
int s2n_hybrid_server_key_send(struct s2n_connection *conn, struct s2n_blob *data_to_sign);
