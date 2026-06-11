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

#include "tls/extensions/s2n_client_alpn.h"

#include <stdint.h>
#include <sys/param.h>

#include "tls/extensions/s2n_extension_type.h"
#include "tls/s2n_protocol_preferences.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls_parameters.h"
#include "utils/s2n_safety.h"

bool s2n_client_alpn_should_send(struct s2n_connection *conn);
static int s2n_client_alpn_send(struct s2n_connection *conn, struct s2n_stuffer *out);
static int s2n_client_alpn_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);

const s2n_extension_type s2n_client_alpn_extension = {
    .iana_value = TLS_EXTENSION_ALPN,
    .is_response = false,
    .send = s2n_client_alpn_send,
    .recv = s2n_client_alpn_recv,
    .should_send = s2n_client_alpn_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};

bool s2n_client_alpn_should_send(struct s2n_connection *conn)
{
    struct s2n_blob *client_app_protocols = NULL;

    return s2n_connection_get_protocol_preferences(conn, &client_app_protocols) == S2N_SUCCESS
            && client_app_protocols->size != 0 && client_app_protocols->data != NULL;
}

static int s2n_client_alpn_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    struct s2n_blob *client_app_protocols = NULL;
    POSIX_GUARD(s2n_connection_get_protocol_preferences(conn, &client_app_protocols));
    POSIX_ENSURE_REF(client_app_protocols);

    POSIX_GUARD(s2n_stuffer_write_uint16(out, client_app_protocols->size));
    POSIX_GUARD(s2n_stuffer_write(out, client_app_protocols));

    return S2N_SUCCESS;
}

static int s2n_client_alpn_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    struct s2n_blob *supported_protocols = NULL;
    POSIX_GUARD(s2n_connection_get_protocol_preferences(conn, &supported_protocols));
    POSIX_ENSURE_REF(supported_protocols);

    if (supported_protocols->size == 0) {
        /* No protocols configured, nothing to do */
        return S2N_SUCCESS;
    }

    uint16_t wire_size = 0;
    POSIX_GUARD(s2n_stuffer_read_uint16(extension, &wire_size));
    if (wire_size > s2n_stuffer_data_available(extension) || wire_size < 3) {
        /* Malformed length, ignore the extension */
        return S2N_SUCCESS;
    }

    struct s2n_blob client_protocols = { 0 };
    POSIX_GUARD(s2n_blob_init(&client_protocols, s2n_stuffer_raw_read(extension, wire_size), wire_size));

    struct s2n_stuffer server_protocols = { 0 };
    POSIX_GUARD(s2n_stuffer_init(&server_protocols, supported_protocols));
    POSIX_GUARD(s2n_stuffer_skip_write(&server_protocols, supported_protocols->size));

    POSIX_GUARD_RESULT(s2n_select_server_preference_protocol(conn, &server_protocols, &client_protocols));

    return S2N_SUCCESS;
}
