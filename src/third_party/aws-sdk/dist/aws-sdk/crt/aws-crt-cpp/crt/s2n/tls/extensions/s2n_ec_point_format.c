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

#include "tls/extensions/s2n_ec_point_format.h"

#include <stdint.h>
#include <sys/param.h>

#include "tls/extensions/s2n_client_supported_groups.h"
#include "tls/s2n_tls.h"
#include "utils/s2n_safety.h"

static int s2n_ec_point_format_send(struct s2n_connection *conn, struct s2n_stuffer *out);
static int s2n_ec_point_format_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);

const s2n_extension_type s2n_client_ec_point_format_extension = {
    .iana_value = TLS_EXTENSION_EC_POINT_FORMATS,
    .is_response = false,
    .send = s2n_ec_point_format_send,
    .recv = s2n_ec_point_format_recv,
    .should_send = s2n_extension_should_send_if_ecc_enabled,
    .if_missing = s2n_extension_noop_if_missing,
};

static bool s2n_server_ec_point_format_should_send(struct s2n_connection *conn);

const s2n_extension_type s2n_server_ec_point_format_extension = {
    .iana_value = TLS_EXTENSION_EC_POINT_FORMATS,
    .is_response = true,
    .send = s2n_ec_point_format_send,
    .recv = s2n_extension_recv_noop,
    .should_send = s2n_server_ec_point_format_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};

static bool s2n_server_ec_point_format_should_send(struct s2n_connection *conn)
{
    return conn && conn->secure && conn->secure->cipher_suite
            && s2n_kex_includes(conn->secure->cipher_suite->key_exchange_alg, &s2n_ecdhe);
}

static int s2n_ec_point_format_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    /* Point format list len. We only support one. */
    POSIX_GUARD(s2n_stuffer_write_uint8(out, 1));

    /* Only allow uncompressed format */
    POSIX_GUARD(s2n_stuffer_write_uint8(out, TLS_EC_POINT_FORMAT_UNCOMPRESSED));

    return S2N_SUCCESS;
}

static int s2n_ec_point_format_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    /* Only uncompressed points are supported by the server and the client must include it in
     * the extension. Just skip the extension.
     *
     *= https://www.rfc-editor.org/rfc/rfc8422#section-5.1.2
     *= type=exception
     *= reason=Incorrect implementations exist in the wild. Skipping validation.
     *# If the client sends the extension and the extension does not contain
     *# the uncompressed point format, and the client has used the Supported
     *# Groups extension to indicate support for any of the curves defined in
     *# this specification, then the server MUST abort the handshake and
     *# return an illegal_parameter alert.
     */
    conn->ec_point_formats = 1;
    return S2N_SUCCESS;
}
