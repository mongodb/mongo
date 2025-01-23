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

#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_tls.h"
#include "utils/s2n_safety.h"

S2N_RESULT s2n_calculate_padding(uint8_t protocol_len, uint8_t *padding_len)
{
    RESULT_ENSURE_REF(padding_len);

    /*
     *= https://datatracker.ietf.org/doc/id/draft-agl-tls-nextprotoneg-03#section-3
     *# The length of "padding" SHOULD be 32 - ((len(selected_protocol) + 2) % 32).
     */
    *padding_len = 32 - (((uint16_t) protocol_len + 2) % 32);
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_write_npn_protocol(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    RESULT_ENSURE_REF(conn);

    uint8_t protocol_len = strlen(conn->application_protocol);
    RESULT_GUARD_POSIX(s2n_stuffer_write_uint8(out, protocol_len));
    RESULT_GUARD_POSIX(s2n_stuffer_write_bytes(out, (uint8_t *) conn->application_protocol, protocol_len));

    uint8_t padding_len = 0;
    RESULT_GUARD(s2n_calculate_padding(protocol_len, &padding_len));
    RESULT_GUARD_POSIX(s2n_stuffer_write_uint8(out, padding_len));
    uint8_t *data_ptr = s2n_stuffer_raw_write(out, padding_len);
    RESULT_ENSURE_REF(data_ptr);
    RESULT_CHECKED_MEMSET(data_ptr, 0, padding_len);

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_read_npn_protocol(struct s2n_connection *conn, struct s2n_stuffer *in)
{
    RESULT_ENSURE_REF(conn);

    uint8_t protocol_len = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint8(in, &protocol_len));

    uint8_t *protocol = s2n_stuffer_raw_read(in, protocol_len);
    RESULT_ENSURE_REF(protocol);
    RESULT_CHECKED_MEMCPY(conn->application_protocol, protocol, protocol_len);
    conn->application_protocol[protocol_len] = '\0';

    uint8_t expected_padding_len = 0;
    RESULT_GUARD(s2n_calculate_padding(protocol_len, &expected_padding_len));
    uint8_t padding_len = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint8(in, &padding_len));
    RESULT_ENSURE_EQ(padding_len, expected_padding_len);

    uint8_t *data_ptr = s2n_stuffer_raw_read(in, padding_len);
    RESULT_ENSURE_REF(data_ptr);
    uint8_t empty_array[UINT8_MAX] = { 0 };
    RESULT_ENSURE_EQ(s2n_constant_time_equals(data_ptr, empty_array, padding_len), 1);

    RESULT_ENSURE_EQ(s2n_stuffer_data_available(in), 0);

    return S2N_RESULT_OK;
}

int s2n_next_protocol_send(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE(conn->actual_protocol_version < S2N_TLS13, S2N_ERR_BAD_MESSAGE);

    struct s2n_stuffer *out = &conn->handshake.io;
    POSIX_GUARD_RESULT(s2n_write_npn_protocol(conn, out));

    POSIX_GUARD_RESULT(s2n_crypto_parameters_switch(conn));

    return S2N_SUCCESS;
}

int s2n_next_protocol_recv(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE(conn->actual_protocol_version < S2N_TLS13, S2N_ERR_BAD_MESSAGE);

    struct s2n_stuffer *in = &conn->handshake.io;
    POSIX_GUARD_RESULT(s2n_read_npn_protocol(conn, in));

    return S2N_SUCCESS;
}
