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

#include "api/s2n.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/extensions/s2n_early_data_indication.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_early_data.h"
#include "utils/s2n_safety.h"

static bool s2n_nst_early_data_indication_should_send(struct s2n_connection *conn)
{
    uint32_t server_max_early_data = 0;
    return s2n_result_is_ok(s2n_early_data_get_server_max_size(conn, &server_max_early_data))
            && server_max_early_data > 0;
}

/**
 * The client version of this extension is empty, so we don't read/write any data.
 *
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.10
 *# The "extension_data" field of this extension contains an
 *# "EarlyDataIndication" value.
 *#
 *#     struct {} Empty;
 *#
 *#     struct {
 *#         select (Handshake.msg_type) {
 *#             case new_session_ticket:   uint32 max_early_data_size;
 **
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.10
 *#         };
 *#     } EarlyDataIndication;
 **/

static int s2n_nst_early_data_indication_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    uint32_t server_max_early_data = 0;
    POSIX_GUARD_RESULT(s2n_early_data_get_server_max_size(conn, &server_max_early_data));
    POSIX_GUARD(s2n_stuffer_write_uint32(out, server_max_early_data));
    return S2N_SUCCESS;
}

static int s2n_nst_early_data_indiction_recv(struct s2n_connection *conn, struct s2n_stuffer *in)
{
    POSIX_ENSURE_REF(conn);
    uint32_t server_max_early_data = 0;
    POSIX_GUARD(s2n_stuffer_read_uint32(in, &server_max_early_data));
    POSIX_GUARD(s2n_connection_set_server_max_early_data_size(conn, server_max_early_data));
    return S2N_SUCCESS;
}

static int s2n_nst_early_data_indication_missing(struct s2n_connection *conn)
{
    POSIX_GUARD(s2n_connection_set_server_max_early_data_size(conn, 0));
    return S2N_SUCCESS;
}

const s2n_extension_type s2n_nst_early_data_indication_extension = {
    .iana_value = TLS_EXTENSION_EARLY_DATA,
    .is_response = false,
    .send = s2n_nst_early_data_indication_send,
    .recv = s2n_nst_early_data_indiction_recv,
    .should_send = s2n_nst_early_data_indication_should_send,
    .if_missing = s2n_nst_early_data_indication_missing,
};
