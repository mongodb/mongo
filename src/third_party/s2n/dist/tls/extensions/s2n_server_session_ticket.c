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

#include "tls/extensions/s2n_server_session_ticket.h"

#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls_parameters.h"

static bool s2n_session_ticket_should_send(struct s2n_connection *conn);
static int s2n_session_ticket_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);

const s2n_extension_type s2n_server_session_ticket_extension = {
    .iana_value = TLS_EXTENSION_SESSION_TICKET,
    .is_response = true,
    .send = s2n_extension_send_noop,
    .recv = s2n_session_ticket_recv,
    .should_send = s2n_session_ticket_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};

static bool s2n_session_ticket_should_send(struct s2n_connection *conn)
{
    return s2n_server_sending_nst(conn) && s2n_connection_get_protocol_version(conn) < S2N_TLS13
            && !conn->config->ticket_forward_secrecy;
}

static int s2n_session_ticket_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    /* Read nothing. The extension just needs to exist. */
    POSIX_ENSURE_REF(conn);
    conn->session_ticket_status = S2N_NEW_TICKET;
    return S2N_SUCCESS;
}

/* Old-style extension functions -- remove after extensions refactor is complete */

int s2n_recv_server_session_ticket_ext(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    return s2n_extension_recv(&s2n_server_session_ticket_extension, conn, extension);
}
