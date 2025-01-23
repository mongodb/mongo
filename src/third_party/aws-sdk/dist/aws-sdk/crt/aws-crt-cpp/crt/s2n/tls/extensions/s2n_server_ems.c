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

#include <stdint.h>
#include <sys/param.h>

#include "tls/extensions/s2n_ems.h"
#include "tls/s2n_tls.h"
#include "utils/s2n_safety.h"

static int s2n_server_ems_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);
static bool s2n_server_ems_should_send(struct s2n_connection *conn);
static int s2n_server_ems_if_missing(struct s2n_connection *conn);

/**
 *= https://www.rfc-editor.org/rfc/rfc7627#section-5.1
 *#
 *#   This document defines a new TLS extension, "extended_master_secret"
 *#   (with extension type 0x0017), which is used to signal both client and
 *#   server to use the extended master secret computation.  The
 *#   "extension_data" field of this extension is empty.  Thus, the entire
 *#   encoding of the extension is 00 17 00 00 (in hexadecimal.)
 **/
const s2n_extension_type s2n_server_ems_extension = {
    .iana_value = TLS_EXTENSION_EMS,
    .is_response = true,
    .send = s2n_extension_send_noop,
    .recv = s2n_server_ems_recv,
    .should_send = s2n_server_ems_should_send,
    .if_missing = s2n_server_ems_if_missing,
};

static int s2n_server_ems_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    POSIX_ENSURE_REF(conn);

    /* Read nothing. The extension just needs to exist without any data. */
    POSIX_ENSURE(s2n_stuffer_data_available(extension) == 0, S2N_ERR_UNSUPPORTED_EXTENSION);
    conn->ems_negotiated = true;

    return S2N_SUCCESS;
}

static bool s2n_server_ems_should_send(struct s2n_connection *conn)
{
    return conn && conn->actual_protocol_version < S2N_TLS13;
}

static int s2n_server_ems_if_missing(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);

    /**
     *= https://www.rfc-editor.org/rfc/rfc7627#section-5.3
     *#    If the original session used the extension but the new ServerHello
     *#    does not contain the extension, the client MUST abort the
     *#    handshake.
     **/
    POSIX_ENSURE(!conn->ems_negotiated, S2N_ERR_MISSING_EXTENSION);

    return S2N_SUCCESS;
}
