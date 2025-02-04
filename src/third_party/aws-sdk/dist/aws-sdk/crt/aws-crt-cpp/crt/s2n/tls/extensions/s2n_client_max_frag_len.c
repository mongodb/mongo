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

#include "tls/extensions/s2n_client_max_frag_len.h"

#include <stdint.h>
#include <sys/param.h>

#include "tls/s2n_tls.h"
#include "tls/s2n_tls_parameters.h"
#include "utils/s2n_safety.h"

static bool s2n_client_max_frag_len_should_send(struct s2n_connection *conn);
static int s2n_client_max_frag_len_send(struct s2n_connection *conn, struct s2n_stuffer *out);
static int s2n_client_max_frag_len_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);

const s2n_extension_type s2n_client_max_frag_len_extension = {
    .iana_value = TLS_EXTENSION_MAX_FRAG_LEN,
    .is_response = false,
    .send = s2n_client_max_frag_len_send,
    .recv = s2n_client_max_frag_len_recv,
    .should_send = s2n_client_max_frag_len_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};

static bool s2n_client_max_frag_len_should_send(struct s2n_connection *conn)
{
    return conn->config->mfl_code != S2N_TLS_MAX_FRAG_LEN_EXT_NONE;
}

static int s2n_client_max_frag_len_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    return s2n_stuffer_write_uint8(out, conn->config->mfl_code);
}

static int s2n_client_max_frag_len_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    if (!conn->config->accept_mfl) {
        return S2N_SUCCESS;
    }

    uint8_t mfl_code = 0;
    POSIX_GUARD(s2n_stuffer_read_uint8(extension, &mfl_code));

    /*
     *= https://www.rfc-editor.org/rfc/rfc6066#section-4
     *= type=exception
     *= reason=For compatibility, we choose to ignore malformed extensions if they are optional
     *# If a server receives a maximum fragment length negotiation request
     *# for a value other than the allowed values, it MUST abort the
     *# handshake with an "illegal_parameter" alert.
     */
    if (mfl_code >= s2n_array_len(mfl_code_to_length) || mfl_code_to_length[mfl_code] > S2N_TLS_MAXIMUM_FRAGMENT_LENGTH) {
        return S2N_SUCCESS;
    }

    /*
     *= https://www.rfc-editor.org/rfc/rfc6066#section-4
     *# Once a maximum fragment length other than 2^14 has been successfully
     *# negotiated, the client and server MUST immediately begin fragmenting
     *# messages (including handshake messages) to ensure that no fragment
     *# larger than the negotiated length is sent.
     */
    conn->negotiated_mfl_code = mfl_code;
    POSIX_GUARD_RESULT(s2n_connection_set_max_fragment_length(conn, mfl_code_to_length[mfl_code]));
    return S2N_SUCCESS;
}
