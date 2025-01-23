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

#include "tls/extensions/s2n_cookie.h"
#include "tls/s2n_tls.h"
#include "utils/s2n_random.h"

/*
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.2
 *# When sending the new ClientHello, the client MUST copy
 *# the contents of the extension received in the HelloRetryRequest into
 *# a "cookie" extension in the new ClientHello.
 *
 * If the server sent a cookie, send it back.
 */
static bool s2n_client_cookie_should_send(struct s2n_connection *conn)
{
    return conn && conn->cookie.size > 0;
}

/*
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.2
 *#   struct {
 *#       opaque cookie<1..2^16-1>;
 *#   } Cookie;
 */
int s2n_cookie_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(conn);

    POSIX_GUARD(s2n_stuffer_write_uint16(out, conn->cookie.size));
    POSIX_GUARD(s2n_stuffer_write(out, &conn->cookie));

    return S2N_SUCCESS;
}

/*
 * Our server does not send cookies in production, so
 * should never receive a cookie back from the client.
 *
 * However, we may enable cookies for testing.
 * In that case, verify the proper client behavior by
 * checking that the cookie sent matches the cookie received.
 * This is also why the "if_missing" behavior is an error.
 */
static int s2n_client_cookie_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE(s2n_in_unit_test(), S2N_ERR_UNSUPPORTED_EXTENSION);

    uint16_t size = 0;
    POSIX_GUARD(s2n_stuffer_read_uint16(extension, &size));
    POSIX_ENSURE(size == conn->cookie.size, S2N_ERR_BAD_MESSAGE);
    POSIX_ENSURE(size >= s2n_stuffer_data_available(extension), S2N_ERR_BAD_MESSAGE);

    uint8_t *cookie = s2n_stuffer_raw_read(extension, size);
    POSIX_ENSURE_REF(cookie);
    POSIX_ENSURE(s2n_constant_time_equals(cookie, conn->cookie.data, size), S2N_ERR_BAD_MESSAGE);

    return S2N_SUCCESS;
}

const s2n_extension_type s2n_client_cookie_extension = {
    .iana_value = TLS_EXTENSION_COOKIE,
    .minimum_version = S2N_TLS13,
    .is_response = true,
    .send = s2n_cookie_send,
    .recv = s2n_client_cookie_recv,
    .should_send = s2n_client_cookie_should_send,
    .if_missing = s2n_extension_error_if_missing,
};
