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

#pragma once

#include "utils/s2n_result.h"

/* Maximum number of valid handshakes */
#define S2N_HANDSHAKES_COUNT 256

#define IS_NEGOTIATED(conn) \
    (s2n_handshake_type_check_flag(conn, NEGOTIATED))

#define IS_FULL_HANDSHAKE(conn) \
    (s2n_handshake_type_check_flag(conn, FULL_HANDSHAKE))

#define IS_RESUMPTION_HANDSHAKE(conn) \
    (!IS_FULL_HANDSHAKE(conn) && IS_NEGOTIATED(conn))

#define IS_CLIENT_AUTH_HANDSHAKE(conn) \
    (s2n_handshake_type_check_flag(conn, CLIENT_AUTH))

#define IS_CLIENT_AUTH_NO_CERT(conn) \
    (IS_CLIENT_AUTH_HANDSHAKE(conn) && s2n_handshake_type_check_flag(conn, NO_CLIENT_CERT))

#define IS_TLS12_PERFECT_FORWARD_SECRECY_HANDSHAKE(conn) \
    (s2n_handshake_type_check_tls12_flag(conn, TLS12_PERFECT_FORWARD_SECRECY))

#define IS_OCSP_STAPLED(conn) \
    (s2n_handshake_type_check_tls12_flag(conn, OCSP_STATUS))

#define IS_ISSUING_NEW_SESSION_TICKET(conn) \
    (s2n_handshake_type_check_tls12_flag(conn, WITH_SESSION_TICKET))

#define IS_NPN_HANDSHAKE(conn) \
    (s2n_handshake_type_check_tls12_flag(conn, WITH_NPN))

#define IS_HELLO_RETRY_HANDSHAKE(conn) \
    (s2n_handshake_type_check_tls13_flag(conn, HELLO_RETRY_REQUEST))

#define IS_MIDDLEBOX_COMPAT_MODE(conn) \
    (s2n_handshake_type_check_tls13_flag(conn, MIDDLEBOX_COMPAT))

#define WITH_EARLY_DATA(conn) \
    (s2n_handshake_type_check_tls13_flag(conn, WITH_EARLY_DATA))

#define WITH_EARLY_CLIENT_CCS(conn) \
    (s2n_handshake_type_check_tls13_flag(conn, EARLY_CLIENT_CCS))

typedef enum {
    INITIAL = 0,
    NEGOTIATED = 1,
    FULL_HANDSHAKE = 2,
    CLIENT_AUTH = 4,
    NO_CLIENT_CERT = 8,
} s2n_handshake_type_flag;

S2N_RESULT s2n_handshake_type_set_flag(struct s2n_connection *conn, s2n_handshake_type_flag flag);
bool s2n_handshake_type_check_flag(struct s2n_connection *conn, s2n_handshake_type_flag flag);

typedef enum {
    TLS12_PERFECT_FORWARD_SECRECY = 16,
    OCSP_STATUS = 32,
    WITH_SESSION_TICKET = 64,
    WITH_NPN = 128,
} s2n_tls12_handshake_type_flag;

S2N_RESULT s2n_handshake_type_set_tls12_flag(struct s2n_connection *conn, s2n_tls12_handshake_type_flag flag);
S2N_RESULT s2n_handshake_type_unset_tls12_flag(struct s2n_connection *conn, s2n_tls12_handshake_type_flag flag);
bool s2n_handshake_type_check_tls12_flag(struct s2n_connection *conn, s2n_tls12_handshake_type_flag flag);

typedef enum {
    HELLO_RETRY_REQUEST = 16,
    MIDDLEBOX_COMPAT = 32,
    WITH_EARLY_DATA = 64,
    EARLY_CLIENT_CCS = 128,
} s2n_tls13_handshake_type_flag;

S2N_RESULT s2n_handshake_type_set_tls13_flag(struct s2n_connection *conn, s2n_tls13_handshake_type_flag flag);
bool s2n_handshake_type_check_tls13_flag(struct s2n_connection *conn, s2n_tls13_handshake_type_flag flag);

S2N_RESULT s2n_handshake_type_reset(struct s2n_connection *conn);
