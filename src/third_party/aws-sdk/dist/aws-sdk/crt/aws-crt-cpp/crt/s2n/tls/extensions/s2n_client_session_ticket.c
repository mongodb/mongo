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

#include "tls/extensions/s2n_client_session_ticket.h"

#include <stdint.h>
#include <sys/param.h>

#include "tls/extensions/s2n_client_psk.h"
#include "tls/s2n_resume.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls_parameters.h"
#include "utils/s2n_safety.h"

static bool s2n_client_session_ticket_should_send(struct s2n_connection *conn);
static int s2n_client_session_ticket_send(struct s2n_connection *conn, struct s2n_stuffer *out);
static int s2n_client_session_ticket_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);

const s2n_extension_type s2n_client_session_ticket_extension = {
    .iana_value = TLS_EXTENSION_SESSION_TICKET,
    .is_response = false,
    .send = s2n_client_session_ticket_send,
    .recv = s2n_client_session_ticket_recv,
    .should_send = s2n_client_session_ticket_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};

static bool s2n_client_session_ticket_should_send(struct s2n_connection *conn)
{
    return conn->config->use_tickets && !conn->config->ticket_forward_secrecy
            && !s2n_client_psk_should_send(conn);
}

static int s2n_client_session_ticket_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_GUARD(s2n_stuffer_write(out, &conn->client_ticket));
    return S2N_SUCCESS;
}

static int s2n_client_session_ticket_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    if (conn->config->use_tickets != 1 || conn->actual_protocol_version > S2N_TLS12
            || conn->config->ticket_forward_secrecy) {
        /* Ignore the extension. */
        return S2N_SUCCESS;
    }

    /* s2n server does not support session ticket with CLIENT_AUTH enabled */
    if (s2n_connection_is_client_auth_enabled(conn) > 0) {
        return S2N_SUCCESS;
    }

    if (s2n_stuffer_data_available(extension) == S2N_TLS12_TICKET_SIZE_IN_BYTES) {
        conn->session_ticket_status = S2N_DECRYPT_TICKET;
        POSIX_GUARD(s2n_stuffer_copy(extension, &conn->client_ticket_to_decrypt, S2N_TLS12_TICKET_SIZE_IN_BYTES));
    } else if (s2n_result_is_ok(s2n_config_is_encrypt_key_available(conn->config))) {
        conn->session_ticket_status = S2N_NEW_TICKET;
    }

    return S2N_SUCCESS;
}
