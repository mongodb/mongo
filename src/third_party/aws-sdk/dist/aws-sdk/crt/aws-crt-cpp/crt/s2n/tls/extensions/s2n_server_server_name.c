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

#include "tls/extensions/s2n_server_server_name.h"

#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_connection.h"

static bool s2n_server_name_should_send(struct s2n_connection *conn);
static int s2n_server_name_send(struct s2n_connection *conn, struct s2n_stuffer *out);
static int s2n_server_name_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);

const s2n_extension_type s2n_server_server_name_extension = {
    .iana_value = TLS_EXTENSION_SERVER_NAME,
    .is_response = true,
    .send = s2n_server_name_send,
    .recv = s2n_server_name_recv,
    .should_send = s2n_server_name_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};

static bool s2n_server_name_should_send(struct s2n_connection *conn)
{
    return conn && conn->server_name_used && !IS_RESUMPTION_HANDSHAKE(conn);
}

static int s2n_server_name_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    /* Write nothing. The extension just needs to exist. */
    return S2N_SUCCESS;
}

static int s2n_server_name_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    POSIX_ENSURE_REF(conn);
    /* Read nothing. The extension just needs to exist. */
    conn->server_name_used = 1;
    return S2N_SUCCESS;
}
