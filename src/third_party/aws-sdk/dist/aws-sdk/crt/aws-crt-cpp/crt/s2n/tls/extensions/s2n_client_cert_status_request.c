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

#include "tls/extensions/s2n_client_cert_status_request.h"

#include <stdint.h>
#include <sys/param.h>

#include "tls/s2n_tls.h"
#include "tls/s2n_tls_parameters.h"
#include "utils/s2n_safety.h"

static bool s2n_client_cert_status_request_should_send(struct s2n_connection *conn);
static int s2n_client_cert_status_request_send(struct s2n_connection *conn, struct s2n_stuffer *out);
static int s2n_client_cert_status_request_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);

const s2n_extension_type s2n_client_cert_status_request_extension = {
    .iana_value = TLS_EXTENSION_STATUS_REQUEST,
    .is_response = false,
    .send = s2n_client_cert_status_request_send,
    .recv = s2n_client_cert_status_request_recv,
    .should_send = s2n_client_cert_status_request_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};

static bool s2n_client_cert_status_request_should_send(struct s2n_connection *conn)
{
    return conn->request_ocsp_status;
}

static int s2n_client_cert_status_request_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_GUARD(s2n_stuffer_write_uint8(out, (uint8_t) S2N_STATUS_REQUEST_OCSP));

    /* responder_id_list
     *
     * From https://tools.ietf.org/html/rfc6066#section-8:
     * A zero-length "responder_id_list" sequence has the special meaning that the responders are implicitly
     * known to the server, e.g., by prior arrangement */
    POSIX_GUARD(s2n_stuffer_write_uint16(out, 0));

    /* request_extensions
     *
     * From https://tools.ietf.org/html/rfc6066#section-8:
     * A zero-length "request_extensions" value means that there are no extensions. */
    POSIX_GUARD(s2n_stuffer_write_uint16(out, 0));

    return S2N_SUCCESS;
}

static int s2n_client_cert_status_request_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    if (s2n_stuffer_data_available(extension) < 5) {
        /* Malformed length, ignore the extension */
        return S2N_SUCCESS;
    }

    uint8_t type = 0;
    POSIX_GUARD(s2n_stuffer_read_uint8(extension, &type));
    if (type != (uint8_t) S2N_STATUS_REQUEST_OCSP) {
        /* We only support OCSP (type 1), ignore the extension */
        return S2N_SUCCESS;
    }

    conn->status_type = (s2n_status_request_type) type;
    return S2N_SUCCESS;
}
