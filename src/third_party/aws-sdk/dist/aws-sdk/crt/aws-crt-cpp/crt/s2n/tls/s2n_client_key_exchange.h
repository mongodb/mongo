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
#include "utils/s2n_blob.h"

int s2n_dhe_client_key_send(struct s2n_connection *conn, struct s2n_blob *shared_key);
int s2n_ecdhe_client_key_send(struct s2n_connection *conn, struct s2n_blob *shared_key);
int s2n_rsa_client_key_send(struct s2n_connection *conn, struct s2n_blob *shared_key);
int s2n_kem_client_key_send(struct s2n_connection *conn, struct s2n_blob *shared_key);
int s2n_hybrid_client_key_send(struct s2n_connection *conn, struct s2n_blob *shared_key);

int s2n_dhe_client_key_recv(struct s2n_connection *conn, struct s2n_blob *shared_key);
int s2n_ecdhe_client_key_recv(struct s2n_connection *conn, struct s2n_blob *shared_key);
int s2n_rsa_client_key_recv(struct s2n_connection *conn, struct s2n_blob *shared_key);
int s2n_kem_client_key_recv(struct s2n_connection *conn, struct s2n_blob *shared_key);
int s2n_hybrid_client_key_recv(struct s2n_connection *conn, struct s2n_blob *shared_key);

int s2n_dhe_client_key_external(struct s2n_connection *conn, struct s2n_blob *shared_key);
int s2n_ecdhe_client_key_external(struct s2n_connection *conn, struct s2n_blob *shared_key);
int s2n_rsa_client_key_external(struct s2n_connection *conn, struct s2n_blob *shared_key);
