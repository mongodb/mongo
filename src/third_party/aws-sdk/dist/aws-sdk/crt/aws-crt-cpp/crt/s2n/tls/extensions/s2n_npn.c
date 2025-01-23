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

#include "tls/extensions/s2n_npn.h"

#include "tls/extensions/s2n_client_alpn.h"
#include "tls/extensions/s2n_server_alpn.h"
#include "tls/s2n_protocol_preferences.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls_parameters.h"
#include "utils/s2n_safety.h"

bool s2n_npn_should_send(struct s2n_connection *conn)
{
    /*
     *= https://datatracker.ietf.org/doc/id/draft-agl-tls-nextprotoneg-03#section-3
     *# For the same reasons, after a handshake has been performed for a
     *# given connection, renegotiations on the same connection MUST NOT
     *# include the "next_protocol_negotiation" extension.
     */
    return s2n_client_alpn_should_send(conn) && conn->config->npn_supported && !s2n_handshake_is_renegotiation(conn);
}

int s2n_client_npn_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    /* Only use the NPN extension to negotiate a protocol if we don't have
     * an option to use the ALPN extension.
     */
    if (s2n_npn_should_send(conn) && !s2n_server_alpn_should_send(conn)) {
        conn->npn_negotiated = true;
    }

    return S2N_SUCCESS;
}

const s2n_extension_type s2n_client_npn_extension = {
    .iana_value = TLS_EXTENSION_NPN,
    .is_response = false,
    .send = s2n_extension_send_noop,
    .recv = s2n_client_npn_recv,
    .should_send = s2n_npn_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};

bool s2n_server_npn_should_send(struct s2n_connection *conn)
{
    return conn->npn_negotiated;
}

int s2n_server_npn_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    struct s2n_blob *app_protocols = NULL;
    POSIX_GUARD(s2n_connection_get_protocol_preferences(conn, &app_protocols));
    POSIX_ENSURE_REF(app_protocols);

    POSIX_GUARD(s2n_stuffer_write(out, app_protocols));

    return S2N_SUCCESS;
}

int s2n_server_npn_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    struct s2n_blob *supported_protocols = NULL;
    POSIX_GUARD(s2n_connection_get_protocol_preferences(conn, &supported_protocols));
    POSIX_ENSURE_REF(supported_protocols);

    if (supported_protocols->size == 0) {
        /* No protocols configured */
        return S2N_SUCCESS;
    }

    /*
     *= https://datatracker.ietf.org/doc/id/draft-agl-tls-nextprotoneg-03#section-3
     *# The "extension_data" field of a "next_protocol_negotiation" extension
     *# in a "ServerHello" contains an optional list of protocols advertised
     *# by the server.
     */
    if (s2n_stuffer_data_available(extension)) {
        POSIX_GUARD_RESULT(s2n_select_server_preference_protocol(conn, extension, supported_protocols));
    }

    /*
     *= https://datatracker.ietf.org/doc/id/draft-agl-tls-nextprotoneg-03#section-4
     *# In the event that the client doesn't support any of server's protocols, or
     *# the server doesn't advertise any, it SHOULD select the first protocol
     *# that it supports.
     */
    if (s2n_get_application_protocol(conn) == NULL) {
        struct s2n_stuffer stuffer = { 0 };
        POSIX_GUARD(s2n_stuffer_init(&stuffer, supported_protocols));
        POSIX_GUARD(s2n_stuffer_skip_write(&stuffer, supported_protocols->size));
        struct s2n_blob protocol = { 0 };
        POSIX_GUARD_RESULT(s2n_protocol_preferences_read(&stuffer, &protocol));

        POSIX_ENSURE_LT(protocol.size, sizeof(conn->application_protocol));
        POSIX_CHECKED_MEMCPY(conn->application_protocol, protocol.data, protocol.size);
        conn->application_protocol[protocol.size] = '\0';
    }

    conn->npn_negotiated = true;

    return S2N_SUCCESS;
}

const s2n_extension_type s2n_server_npn_extension = {
    .iana_value = TLS_EXTENSION_NPN,
    .is_response = true,
    .send = s2n_server_npn_send,
    .recv = s2n_server_npn_recv,
    .should_send = s2n_server_npn_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};
