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

#include "tls/extensions/s2n_client_sct_list.h"

#include <stdint.h>
#include <sys/param.h>

#include "tls/s2n_tls.h"
#include "tls/s2n_tls_parameters.h"
#include "utils/s2n_safety.h"

static bool s2n_client_sct_list_should_send(struct s2n_connection *conn);
static int s2n_client_sct_list_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);

const s2n_extension_type s2n_client_sct_list_extension = {
    .iana_value = TLS_EXTENSION_SCT_LIST,
    .is_response = false,
    .send = s2n_extension_send_noop,
    .recv = s2n_client_sct_list_recv,
    .should_send = s2n_client_sct_list_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};

static bool s2n_client_sct_list_should_send(struct s2n_connection *conn)
{
    return conn->config->ct_type != S2N_CT_SUPPORT_NONE;
}

static int s2n_client_sct_list_recv(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    conn->ct_level_requested = S2N_CT_SUPPORT_REQUEST;
    /* Skip reading the extension, per RFC6962 (3.1.1) it SHOULD be empty anyway  */
    return S2N_SUCCESS;
}
