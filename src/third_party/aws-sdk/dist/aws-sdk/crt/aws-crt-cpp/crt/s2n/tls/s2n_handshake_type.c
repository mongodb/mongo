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

#include "tls/s2n_handshake_type.h"

#include "tls/s2n_connection.h"
#include "utils/s2n_safety.h"

S2N_RESULT s2n_handshake_type_set_flag(struct s2n_connection *conn, s2n_handshake_type_flag flag)
{
    RESULT_ENSURE_REF(conn);
    conn->handshake.handshake_type |= flag;
    return S2N_RESULT_OK;
}

bool s2n_handshake_type_check_flag(struct s2n_connection *conn, s2n_handshake_type_flag flag)
{
    return conn && (conn->handshake.handshake_type & flag);
}

S2N_RESULT s2n_handshake_type_set_tls12_flag(struct s2n_connection *conn, s2n_tls12_handshake_type_flag flag)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE(s2n_connection_get_protocol_version(conn) < S2N_TLS13, S2N_ERR_HANDSHAKE_STATE);
    conn->handshake.handshake_type |= flag;
    RESULT_GUARD(s2n_conn_choose_state_machine(conn, S2N_TLS12));
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_handshake_type_unset_tls12_flag(struct s2n_connection *conn, s2n_tls12_handshake_type_flag flag)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE(s2n_connection_get_protocol_version(conn) < S2N_TLS13, S2N_ERR_HANDSHAKE_STATE);
    conn->handshake.handshake_type &= ~(flag);
    return S2N_RESULT_OK;
}

bool s2n_handshake_type_check_tls12_flag(struct s2n_connection *conn, s2n_tls12_handshake_type_flag flag)
{
    return conn && s2n_connection_get_protocol_version(conn) < S2N_TLS13
            && (conn->handshake.handshake_type & flag);
}

S2N_RESULT s2n_handshake_type_set_tls13_flag(struct s2n_connection *conn, s2n_tls13_handshake_type_flag flag)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE(s2n_connection_get_protocol_version(conn) >= S2N_TLS13, S2N_ERR_HANDSHAKE_STATE);
    conn->handshake.handshake_type |= flag;
    RESULT_GUARD(s2n_conn_choose_state_machine(conn, S2N_TLS13));
    return S2N_RESULT_OK;
}

bool s2n_handshake_type_check_tls13_flag(struct s2n_connection *conn, s2n_tls13_handshake_type_flag flag)
{
    return s2n_connection_get_protocol_version(conn) >= S2N_TLS13
            && (conn->handshake.handshake_type & flag);
}

S2N_RESULT s2n_handshake_type_reset(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);
    conn->handshake.handshake_type = 0;
    return S2N_RESULT_OK;
}
