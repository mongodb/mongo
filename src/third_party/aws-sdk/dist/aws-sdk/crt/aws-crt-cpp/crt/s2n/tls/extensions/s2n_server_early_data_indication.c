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

#include "api/s2n.h"
#include "tls/extensions/s2n_early_data_indication.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_early_data.h"
#include "tls/s2n_handshake.h"
#include "utils/s2n_safety.h"

static bool s2n_server_early_data_indication_should_send(struct s2n_connection *conn)
{
    return conn && conn->early_data_state == S2N_EARLY_DATA_ACCEPTED;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.10
 *# A server which receives an "early_data" extension MUST behave in one
 *# of three ways:
 *#
 *# -  Ignore the extension and return a regular 1-RTT response.
 **/
static int s2n_server_early_data_indication_is_missing(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    if (conn->early_data_state == S2N_EARLY_DATA_REQUESTED) {
        POSIX_GUARD_RESULT(s2n_connection_set_early_data_state(conn, S2N_EARLY_DATA_REJECTED));
    }
    return S2N_SUCCESS;
}

/**
 * The server version of this extension is empty, so we don't read/write any data.
 *
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.10
 *# The "extension_data" field of this extension contains an
 *# "EarlyDataIndication" value.
 *#
 *#     struct {} Empty;
 *#
 *#     struct {
 *#         select (Handshake.msg_type) {
 **
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.10
 *#             case encrypted_extensions: Empty;
 *#         };
 *#     } EarlyDataIndication;
 **/

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.10
 *# A server which receives an "early_data" extension MUST behave in one
 *# of three ways:
 *
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.10
 *# -  Return its own "early_data" extension in EncryptedExtensions,
 *#    indicating that it intends to process the early data.
 **/

static int s2n_server_early_data_indication_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    return S2N_SUCCESS;
}

static int s2n_server_early_data_indication_recv(struct s2n_connection *conn, struct s2n_stuffer *in)
{
    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.10
     *# If any of these checks fail, the server MUST NOT respond with the
     *# extension
     **/
    POSIX_ENSURE(s2n_early_data_is_valid_for_connection(conn), S2N_ERR_EARLY_DATA_NOT_ALLOWED);

    POSIX_GUARD_RESULT(s2n_connection_set_early_data_state(conn, S2N_EARLY_DATA_ACCEPTED));

    /* The client does not know for sure whether the server accepted early data until it receives
     * this extension as part of the EncryptedExtensions message, after the handshake type has
     * already been calculated. We'll need to manually update the handshake type.
     */
    conn->handshake.handshake_type |= WITH_EARLY_DATA;

    return S2N_SUCCESS;
}

const s2n_extension_type s2n_server_early_data_indication_extension = {
    .iana_value = TLS_EXTENSION_EARLY_DATA,
    .is_response = true,
    .send = s2n_server_early_data_indication_send,
    .recv = s2n_server_early_data_indication_recv,
    .should_send = s2n_server_early_data_indication_should_send,
    .if_missing = s2n_server_early_data_indication_is_missing,
};
