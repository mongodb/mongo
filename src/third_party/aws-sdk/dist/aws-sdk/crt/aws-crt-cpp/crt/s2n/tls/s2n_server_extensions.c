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

#include "tls/s2n_server_extensions.h"

#include "stuffer/s2n_stuffer.h"
#include "tls/extensions/s2n_extension_list.h"
#include "tls/extensions/s2n_server_supported_versions.h"
#include "tls/s2n_connection.h"
#include "utils/s2n_safety.h"

/* An empty list will just contain the uint16_t list size */
#define S2N_EMPTY_EXTENSION_LIST_SIZE sizeof(uint16_t)

int s2n_server_extensions_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    uint32_t data_available_before_extensions = s2n_stuffer_data_available(out);

    if (s2n_is_hello_retry_message(conn)) {
        POSIX_GUARD(s2n_extension_list_send(S2N_EXTENSION_LIST_HELLO_RETRY_REQUEST, conn, out));
    } else if (conn->actual_protocol_version >= S2N_TLS13) {
        POSIX_GUARD(s2n_extension_list_send(S2N_EXTENSION_LIST_SERVER_HELLO_TLS13, conn, out));
    } else {
        POSIX_GUARD(s2n_extension_list_send(S2N_EXTENSION_LIST_SERVER_HELLO_DEFAULT, conn, out));
    }

    /* The ServerHello extension list size (uint16_t) is NOT written if the list is empty.
     * This is to support older clients written before extensions existed that might fail
     * on any unexpected bytes at the end of the ServerHello.
     *
     * This behavior is outlined in the TLS1.2 RFC: https://tools.ietf.org/html/rfc5246#appendix-A.4.1
     *
     * This behavior does not affect TLS1.3, which always requires at least the supported_version extension
     * so will never produce an empty list.
     */
    if (s2n_stuffer_data_available(out) - data_available_before_extensions == S2N_EMPTY_EXTENSION_LIST_SIZE) {
        POSIX_GUARD(s2n_stuffer_wipe_n(out, S2N_EMPTY_EXTENSION_LIST_SIZE));
    }

    return S2N_SUCCESS;
}

int s2n_server_extensions_recv(struct s2n_connection *conn, struct s2n_stuffer *in)
{
    s2n_parsed_extensions_list parsed_extension_list = { 0 };
    POSIX_GUARD(s2n_extension_list_parse(in, &parsed_extension_list));

    /**
     * Process supported_versions first so that we know which extensions list to use.
     * - If the supported_versions extension exists, then it will set server_protocol_version.
     * - If the supported_versions extension does not exist, then the server_protocol_version will remain
     *   unknown and we will use the default list of allowed extension types.
     **/
    POSIX_GUARD(s2n_extension_process(&s2n_server_supported_versions_extension, conn, &parsed_extension_list));

    if (s2n_is_hello_retry_message(conn)) {
        /**
         *= https://www.rfc-editor.org/rfc/rfc8446#4.1.4
         *# Otherwise, the client MUST process all extensions in the
         *# HelloRetryRequest
         */
        POSIX_GUARD(s2n_extension_list_process(S2N_EXTENSION_LIST_HELLO_RETRY_REQUEST, conn, &parsed_extension_list));
    } else if (conn->server_protocol_version >= S2N_TLS13) {
        POSIX_GUARD(s2n_extension_list_process(S2N_EXTENSION_LIST_SERVER_HELLO_TLS13, conn, &parsed_extension_list));
    } else {
        POSIX_GUARD(s2n_extension_list_process(S2N_EXTENSION_LIST_SERVER_HELLO_DEFAULT, conn, &parsed_extension_list));
    }

    return S2N_SUCCESS;
}
