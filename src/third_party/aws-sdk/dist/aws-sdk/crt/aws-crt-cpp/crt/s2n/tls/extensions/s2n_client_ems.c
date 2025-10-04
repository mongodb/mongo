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

static int s2n_client_ems_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);
static bool s2n_client_ems_should_send(struct s2n_connection *conn);

/**
 *= https://www.rfc-editor.org/rfc/rfc7627#section-5.1
 *#
 *#   This document defines a new TLS extension, "extended_master_secret"
 *#   (with extension type 0x0017), which is used to signal both client and
 *#   server to use the extended master secret computation.  The
 *#   "extension_data" field of this extension is empty.  Thus, the entire
 *#   encoding of the extension is 00 17 00 00 (in hexadecimal.)
 **/
const s2n_extension_type s2n_client_ems_extension = {
    .iana_value = TLS_EXTENSION_EMS,
    .is_response = false,
    .send = s2n_extension_send_noop,
    .recv = s2n_client_ems_recv,
    .should_send = s2n_client_ems_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};

static int s2n_client_ems_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    POSIX_ENSURE_REF(conn);

    /* Read nothing. The extension just needs to exist without data. */
    POSIX_ENSURE(s2n_stuffer_data_available(extension) == 0, S2N_ERR_UNSUPPORTED_EXTENSION);
    conn->ems_negotiated = true;

    return S2N_SUCCESS;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc7627#section-5.3
 *= type=exception
 *# When offering an abbreviated handshake, the client MUST send the
 *# "extended_master_secret" extension in its ClientHello.
 *
 *  We added an exception here in order to prevent a drop in 
 *  session resumption rates during deployment. Eventually clients
 *  will be forced to do a full handshake as sessions expire and pick up EMS at that point.
 **/
static bool s2n_client_ems_should_send(struct s2n_connection *conn)
{
    /* Don't send this extension if the previous session did not negotiate EMS */
    if (conn && conn->set_session && !conn->ems_negotiated) {
        return false;
    } else {
        return true;
    }
}
