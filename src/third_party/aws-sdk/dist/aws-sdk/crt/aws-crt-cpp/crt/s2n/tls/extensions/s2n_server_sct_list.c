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

#include "tls/extensions/s2n_server_sct_list.h"

#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_tls.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_safety.h"

static bool s2n_server_sct_list_should_send(struct s2n_connection *conn);
static int s2n_server_sct_list_send(struct s2n_connection *conn, struct s2n_stuffer *out);
static int s2n_server_sct_list_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);

const s2n_extension_type s2n_server_sct_list_extension = {
    .iana_value = TLS_EXTENSION_SCT_LIST,
    .is_response = true,
    .send = s2n_server_sct_list_send,
    .recv = s2n_server_sct_list_recv,
    .should_send = s2n_server_sct_list_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};

static bool s2n_server_sct_list_should_send(struct s2n_connection *conn)
{
    return s2n_server_can_send_sct_list(conn);
}

int s2n_server_sct_list_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(conn);
    struct s2n_blob *sct_list = &conn->handshake_params.our_chain_and_key->sct_list;

    POSIX_ENSURE_REF(sct_list);
    POSIX_GUARD(s2n_stuffer_write(out, sct_list));

    return S2N_SUCCESS;
}

int s2n_server_sct_list_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    POSIX_ENSURE_REF(conn);

    struct s2n_blob sct_list = { 0 };
    size_t data_available = s2n_stuffer_data_available(extension);
    POSIX_GUARD(s2n_blob_init(&sct_list,
            s2n_stuffer_raw_read(extension, data_available),
            data_available));
    POSIX_ENSURE_REF(sct_list.data);

    POSIX_GUARD(s2n_dup(&sct_list, &conn->ct_response));

    return S2N_SUCCESS;
}
