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

#include "tls/extensions/s2n_server_psk.h"

#include <stdint.h>
#include <sys/param.h>

#include "tls/s2n_tls.h"
#include "utils/s2n_bitmap.h"
#include "utils/s2n_safety.h"

static bool s2n_server_psk_should_send(struct s2n_connection *conn);
static int s2n_server_psk_send(struct s2n_connection *conn, struct s2n_stuffer *out);
static int s2n_server_psk_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);

const s2n_extension_type s2n_server_psk_extension = {
    .iana_value = TLS_EXTENSION_PRE_SHARED_KEY,
    .minimum_version = S2N_TLS13,
    .is_response = true,
    .send = s2n_server_psk_send,
    .recv = s2n_server_psk_recv,
    .should_send = s2n_server_psk_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};

static bool s2n_server_psk_should_send(struct s2n_connection *conn)
{
    /* Only send a server pre_shared_key extension if a chosen PSK is set on the connection */
    return conn && conn->psk_params.chosen_psk;
}

static int s2n_server_psk_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(conn);

    /* Send the index of the chosen PSK that is stored on the connection. */
    POSIX_GUARD(s2n_stuffer_write_uint16(out, conn->psk_params.chosen_psk_wire_index));

    return S2N_SUCCESS;
}

static int s2n_server_psk_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    POSIX_ENSURE_REF(conn);

    /* Currently in s2n, only (EC)DHE key exchange mode is supported.
     * Any other mode selected by the server is invalid because it was not offered by the client.
     * A key_share extension MUST have been received in order to use a pre-shared key in (EC)DHE key exchange mode.
     */
    s2n_extension_type_id key_share_ext_id = s2n_unsupported_extension;
    POSIX_GUARD(s2n_extension_supported_iana_value_to_id(TLS_EXTENSION_KEY_SHARE, &key_share_ext_id));
    POSIX_ENSURE(S2N_CBIT_TEST(conn->extension_responses_received, key_share_ext_id), S2N_ERR_MISSING_EXTENSION);

    /* From RFC section: https://tools.ietf.org/html/rfc8446#section-4.2.8.1
     * Any future values that are allocated must ensure that the transmitted protocol messages
     * unambiguously identify which mode was selected by the server; at present, this is
     * indicated by the presence of the "key_share" in the ServerHello.
     */
    conn->psk_params.psk_ke_mode = S2N_PSK_DHE_KE;

    uint16_t chosen_psk_wire_index = 0;
    POSIX_GUARD(s2n_stuffer_read_uint16(extension, &chosen_psk_wire_index));

    /* From RFC section: https://tools.ietf.org/html/rfc8446#section-4.2.11 
     * Clients MUST verify that the server's selected_identity is within the
     * range supplied by the client.
     */
    POSIX_ENSURE(chosen_psk_wire_index < conn->psk_params.psk_list.len, S2N_ERR_INVALID_ARGUMENT);
    conn->psk_params.chosen_psk_wire_index = chosen_psk_wire_index;

    POSIX_GUARD_RESULT(s2n_array_get(&conn->psk_params.psk_list, conn->psk_params.chosen_psk_wire_index,
            (void **) &conn->psk_params.chosen_psk));

    return S2N_SUCCESS;
}
