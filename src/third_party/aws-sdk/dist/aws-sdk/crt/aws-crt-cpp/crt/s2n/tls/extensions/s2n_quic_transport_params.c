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

#include "tls/extensions/s2n_quic_transport_params.h"

#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_tls.h"
#include "utils/s2n_safety.h"

/*
 * The quic_transport_params extension is required by the QUIC protocol to
 * negotiate additional connection parameters when using S2N.
 *
 * This extension should not be sent or received unless using S2N with QUIC.
 * S2N treats the extension data as opaque bytes and performs no validation.
 */

static bool s2n_quic_transport_params_should_send(struct s2n_connection *conn)
{
    return s2n_connection_is_quic_enabled(conn);
}

static int s2n_quic_transport_params_if_missing(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->config);
    POSIX_ENSURE(!s2n_connection_is_quic_enabled(conn), S2N_ERR_MISSING_EXTENSION);
    return S2N_SUCCESS;
}

static int s2n_quic_transport_params_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(out);

    if (conn->our_quic_transport_parameters.size) {
        POSIX_GUARD(s2n_stuffer_write(out, &conn->our_quic_transport_parameters));
    }
    return S2N_SUCCESS;
}

static int s2n_quic_transport_params_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(extension);
    POSIX_ENSURE_REF(conn->config);
    POSIX_ENSURE(s2n_connection_is_quic_enabled(conn), S2N_ERR_UNSUPPORTED_EXTENSION);

    if (s2n_stuffer_data_available(extension)) {
        POSIX_GUARD(s2n_alloc(&conn->peer_quic_transport_parameters, s2n_stuffer_data_available(extension)));
        POSIX_GUARD(s2n_stuffer_read(extension, &conn->peer_quic_transport_parameters));
    }
    return S2N_SUCCESS;
}

const s2n_extension_type s2n_quic_transport_parameters_extension = {
    .iana_value = TLS_EXTENSION_QUIC_TRANSPORT_PARAMETERS,
    .minimum_version = S2N_TLS13,
    .is_response = false,
    .send = s2n_quic_transport_params_send,
    .recv = s2n_quic_transport_params_recv,
    .should_send = s2n_quic_transport_params_should_send,
    .if_missing = s2n_quic_transport_params_if_missing,
};
