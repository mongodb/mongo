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

#include "tls/extensions/s2n_server_supported_versions.h"

#include <stdint.h>
#include <sys/param.h>

#include "tls/extensions/s2n_supported_versions.h"
#include "tls/s2n_alerts.h"
#include "tls/s2n_cipher_preferences.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls_parameters.h"
#include "utils/s2n_safety.h"

/**
 * Specified in https://tools.ietf.org/html/rfc8446#section-4.2.1
 *
 * "A server which negotiates TLS 1.3 MUST respond by sending a
 * "supported_versions" extension containing the selected version value
 * (0x0304)."
 *
 * Structure:
 * Extension type (2 bytes)
 * Extension size (2 bytes)
 * Selected Version (2 byte)
 **/

static int s2n_server_supported_versions_send(struct s2n_connection *conn, struct s2n_stuffer *out);
static int s2n_server_supported_versions_recv(struct s2n_connection *conn, struct s2n_stuffer *in);

const s2n_extension_type s2n_server_supported_versions_extension = {
    .iana_value = TLS_EXTENSION_SUPPORTED_VERSIONS,
    .is_response = true,
    .send = s2n_server_supported_versions_send,
    .recv = s2n_server_supported_versions_recv,
    .should_send = s2n_extension_send_if_tls13_connection,
    .if_missing = s2n_extension_noop_if_missing,
};

static int s2n_server_supported_versions_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_GUARD(s2n_stuffer_write_uint8(out, conn->server_protocol_version / 10));
    POSIX_GUARD(s2n_stuffer_write_uint8(out, conn->server_protocol_version % 10));

    return S2N_SUCCESS;
}

static int s2n_extensions_server_supported_versions_process(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    uint8_t highest_supported_version = conn->client_protocol_version;
    uint8_t minimum_supported_version = s2n_unknown_protocol_version;
    POSIX_GUARD_RESULT(s2n_connection_get_minimum_supported_version(conn, &minimum_supported_version));
    POSIX_ENSURE(highest_supported_version >= minimum_supported_version, S2N_ERR_PROTOCOL_VERSION_UNSUPPORTED);

    uint8_t server_version_parts[S2N_TLS_PROTOCOL_VERSION_LEN];
    POSIX_GUARD(s2n_stuffer_read_bytes(extension, server_version_parts, S2N_TLS_PROTOCOL_VERSION_LEN));

    uint16_t server_version = (server_version_parts[0] * 10) + server_version_parts[1];

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#4.1.4
     *# The value of selected_version in the HelloRetryRequest
     *# "supported_versions" extension MUST be retained in the ServerHello,
     *# and a client MUST abort the handshake with an "illegal_parameter"
     *# alert if the value changes.
     **/
    if (s2n_is_hello_retry_handshake(conn) && !s2n_is_hello_retry_message(conn)) {
        POSIX_ENSURE(conn->server_protocol_version == server_version,
                S2N_ERR_BAD_MESSAGE);
    }

    POSIX_ENSURE_GTE(server_version, S2N_TLS13);
    POSIX_ENSURE_LTE(server_version, highest_supported_version);
    POSIX_ENSURE_GTE(server_version, minimum_supported_version);

    conn->server_protocol_version = server_version;

    return S2N_SUCCESS;
}

static int s2n_server_supported_versions_recv(struct s2n_connection *conn, struct s2n_stuffer *in)
{
    if (s2n_connection_get_protocol_version(conn) < S2N_TLS13) {
        return S2N_SUCCESS;
    }

    S2N_ERROR_IF(s2n_extensions_server_supported_versions_process(conn, in) < 0, S2N_ERR_BAD_MESSAGE);
    return S2N_SUCCESS;
}

/* Old-style extension functions -- remove after extensions refactor is complete */

int s2n_extensions_server_supported_versions_size(struct s2n_connection *conn)
{
    return 6;
}

int s2n_extensions_server_supported_versions_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    return s2n_extension_recv(&s2n_server_supported_versions_extension, conn, extension);
}
