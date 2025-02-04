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

#include "tls/extensions/s2n_cert_status_response.h"

#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls_parameters.h"

static bool s2n_cert_status_response_should_send(struct s2n_connection *conn);
static int s2n_cert_status_response_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);

const s2n_extension_type s2n_cert_status_response_extension = {
    .iana_value = TLS_EXTENSION_STATUS_REQUEST,
    .is_response = true,
    .send = s2n_extension_send_noop,
    .recv = s2n_cert_status_response_recv,
    .should_send = s2n_cert_status_response_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};

static bool s2n_cert_status_response_should_send(struct s2n_connection *conn)
{
    return s2n_server_can_send_ocsp(conn);
}

int s2n_cert_status_response_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    /* Read nothing. The extension just needs to exist. */
    POSIX_ENSURE_REF(conn);
    conn->status_type = S2N_STATUS_REQUEST_OCSP;
    return S2N_SUCCESS;
}
