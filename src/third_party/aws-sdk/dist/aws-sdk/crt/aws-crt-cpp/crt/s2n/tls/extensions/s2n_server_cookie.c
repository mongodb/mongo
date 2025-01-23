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
 *# Cookies serve two primary purposes:
 *#
 *#    -  Allowing the server to force the client to demonstrate
 *#       reachability at their apparent network address (thus providing a
 *#       measure of DoS protection).  This is primarily useful for
 *#       non-connection-oriented transports (see [RFC6347] for an example
 *#       of this).
 *#
 *#    -  Allowing the server to offload state to the client, thus allowing
 *#       it to send a HelloRetryRequest without storing any state.  The
 *#       server can do this by storing the hash of the ClientHello in the
 *#       HelloRetryRequest cookie (protected with some suitable integrity
 *#       protection algorithm).
 *#
 *# When sending a HelloRetryRequest, the server MAY provide a "cookie"
 *# extension to the client (this is an exception to the usual rule that
 *# the only extensions that may be sent are those that appear in the
 *# ClientHello).
 *
 * So our server does not send cookies in production,
 * because it doesn't support DTLS and isn't stateless.
 *
 * However, we will sometimes send cookies for testing.
 */
static bool s2n_server_cookie_should_send(struct s2n_connection *conn)
{
    return conn && conn->cookie.size > 0 && s2n_in_unit_test();
}

/*
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.2
 *# When sending the new ClientHello, the client MUST copy
 *# the contents of the extension received in the HelloRetryRequest into
 *# a "cookie" extension in the new ClientHello.
 *
 * Store the server's cookie for later use in the new ClientHello.
 */
static int s2n_server_cookie_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    POSIX_ENSURE_REF(conn);

    /* This extension should only appear on the hello retry request extension list,
     * but verify the retry anyway.
     */
    POSIX_ENSURE(s2n_is_hello_retry_handshake(conn), S2N_ERR_UNSUPPORTED_EXTENSION);

    uint16_t size = 0;
    POSIX_GUARD(s2n_stuffer_read_uint16(extension, &size));
    POSIX_ENSURE(s2n_stuffer_data_available(extension) >= size, S2N_ERR_BAD_MESSAGE);

    POSIX_GUARD(s2n_realloc(&conn->cookie, size));
    POSIX_GUARD(s2n_stuffer_read(extension, &conn->cookie));
    return S2N_SUCCESS;
}

const s2n_extension_type s2n_server_cookie_extension = {
    .iana_value = TLS_EXTENSION_COOKIE,
    .minimum_version = S2N_TLS13,
    .is_response = false,
    .send = s2n_cookie_send,
    .recv = s2n_server_cookie_recv,
    .should_send = s2n_server_cookie_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};
