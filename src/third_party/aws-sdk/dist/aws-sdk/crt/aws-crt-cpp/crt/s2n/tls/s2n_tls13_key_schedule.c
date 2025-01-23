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

#include "tls/s2n_prf.h"
#include "tls/s2n_tls13_handshake.h"
#include "utils/s2n_result.h"

/* The state machine refers to the "master" secret as the "application" secret.
 * Let's use that terminology here to match.
 */
#define S2N_APPLICATION_SECRET S2N_MASTER_SECRET

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#appendix-A
 *# The notation "K_{send,recv} = foo" means "set
 *# the send/recv key to the given key".
 */
#define K_send(conn, secret_type) RESULT_GUARD(s2n_tls13_key_schedule_set_key(conn, secret_type, (conn)->mode))
#define K_recv(conn, secret_type) RESULT_GUARD(s2n_tls13_key_schedule_set_key(conn, secret_type, S2N_PEER_MODE((conn)->mode)))

static const struct s2n_blob s2n_zero_length_context = { 0 };

static S2N_RESULT s2n_zero_sequence_number(struct s2n_connection *conn, s2n_mode mode)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(conn->secure);
    struct s2n_blob sequence_number = { 0 };
    RESULT_GUARD(s2n_connection_get_sequence_number(conn, mode, &sequence_number));
    RESULT_GUARD_POSIX(s2n_blob_zero(&sequence_number));
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_tls13_key_schedule_get_keying_material(
        struct s2n_connection *conn, s2n_extract_secret_type_t secret_type,
        s2n_mode mode, struct s2n_blob *iv, struct s2n_blob *key)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(iv);
    RESULT_ENSURE_REF(key);
    RESULT_ENSURE_REF(conn->secure);

    const struct s2n_cipher_suite *cipher_suite = conn->secure->cipher_suite;
    RESULT_ENSURE_REF(cipher_suite);

    const struct s2n_cipher *cipher = NULL;
    RESULT_GUARD(s2n_connection_get_secure_cipher(conn, &cipher));
    RESULT_ENSURE_REF(cipher);

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-7.3
     *# The traffic keying material is generated from the following input
     *# values:
     *#
     *# -  A secret value
     **/
    struct s2n_blob secret = { 0 };
    uint8_t secret_bytes[S2N_TLS13_SECRET_MAX_LEN] = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&secret, secret_bytes, S2N_TLS13_SECRET_MAX_LEN));
    RESULT_GUARD(s2n_tls13_secrets_get(conn, secret_type, mode, &secret));

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-7.3
     *#
     *# -  A purpose value indicating the specific value being generated
     **/
    const struct s2n_blob *key_purpose = &s2n_tls13_label_traffic_secret_key;
    const struct s2n_blob *iv_purpose = &s2n_tls13_label_traffic_secret_iv;

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-7.3
     *#
     *# -  The length of the key being generated
     **/
    const uint32_t key_size = cipher->key_material_size;
    const uint32_t iv_size = S2N_TLS13_FIXED_IV_LEN;

    /*
     * TODO: We should be able to reuse the prf_work_space rather
     * than allocating a new HMAC every time.
     * https://github.com/aws/s2n-tls/issues/3206
     */
    s2n_hmac_algorithm hmac_alg = cipher_suite->prf_alg;
    DEFER_CLEANUP(struct s2n_hmac_state hmac = { 0 }, s2n_hmac_free);
    RESULT_GUARD_POSIX(s2n_hmac_new(&hmac));

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-7.3
     *#
     *# The traffic keying material is generated from an input traffic secret
     *# value using:
     *#
     *# [sender]_write_key = HKDF-Expand-Label(Secret, "key", "", key_length)
     **/
    RESULT_ENSURE_LTE(key_size, key->size);
    key->size = key_size;
    RESULT_GUARD_POSIX(s2n_hkdf_expand_label(&hmac, hmac_alg,
            &secret, key_purpose, &s2n_zero_length_context, key));
    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-7.3
     *# [sender]_write_iv  = HKDF-Expand-Label(Secret, "iv", "", iv_length)
     **/
    RESULT_ENSURE_LTE(iv_size, iv->size);
    iv->size = iv_size;
    RESULT_GUARD_POSIX(s2n_hkdf_expand_label(&hmac, hmac_alg,
            &secret, iv_purpose, &s2n_zero_length_context, iv));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_tls13_key_schedule_set_key(struct s2n_connection *conn, s2n_extract_secret_type_t secret_type, s2n_mode mode)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(conn->secure);

    uint8_t *implicit_iv_data = NULL;
    struct s2n_session_key *session_key = NULL;
    uint8_t key_bytes[S2N_TLS13_SECRET_MAX_LEN] = { 0 };
    if (mode == S2N_CLIENT) {
        implicit_iv_data = conn->secure->client_implicit_iv;
        session_key = &conn->secure->client_key;
        conn->client = conn->secure;
    } else {
        implicit_iv_data = conn->secure->server_implicit_iv;
        session_key = &conn->secure->server_key;
        conn->server = conn->secure;
    }

    struct s2n_blob iv = { 0 };
    struct s2n_blob key = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&iv, implicit_iv_data, S2N_TLS13_FIXED_IV_LEN));
    RESULT_GUARD_POSIX(s2n_blob_init(&key, key_bytes, sizeof(key_bytes)));
    RESULT_GUARD(s2n_tls13_key_schedule_get_keying_material(
            conn, secret_type, mode, &iv, &key));

    const struct s2n_cipher *cipher = NULL;
    RESULT_GUARD(s2n_connection_get_secure_cipher(conn, &cipher));
    RESULT_ENSURE_REF(cipher);

    bool is_sending_secret = (mode == conn->mode);
    if (is_sending_secret) {
        RESULT_GUARD(cipher->set_encryption_key(session_key, &key));
    } else {
        RESULT_GUARD(cipher->set_decryption_key(session_key, &key));
    }

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-5.3
     *# Each sequence number is
     *# set to zero at the beginning of a connection and whenever the key is
     *# changed; the first record transmitted under a particular traffic key
     *# MUST use sequence number 0.
     */
    RESULT_GUARD(s2n_zero_sequence_number(conn, mode));

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_client_key_schedule(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);

    message_type_t message_type = s2n_conn_get_current_message_type(conn);

    /**
     * How client keys are set varies depending on early data state.
     *
     *= https://www.rfc-editor.org/rfc/rfc8446#appendix-A
     *# Actions which are taken only in certain circumstances
     *# are indicated in [].
     */

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#appendix-A.1
     *#                              START <----+
     *#               Send ClientHello |        | Recv HelloRetryRequest
     *#          [K_send = early data] |        |
     */
    if (message_type == CLIENT_HELLO
            && conn->early_data_state == S2N_EARLY_DATA_REQUESTED) {
        K_send(conn, S2N_EARLY_SECRET);
    }
    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#appendix-A.1
     *#                                v        |
     *#           /                 WAIT_SH ----+
     *#           |                    | Recv ServerHello
     *#           |                    | K_recv = handshake
     */
    if (message_type == SERVER_HELLO) {
        K_recv(conn, S2N_HANDSHAKE_SECRET);
    }
    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#appendix-A.1
     *#       Can |                    V
     *#      send |                 WAIT_EE
     *#     early |                    | Recv EncryptedExtensions
     *#      data |           +--------+--------+
     *#           |     Using |                 | Using certificate
     *#           |       PSK |                 v
     *#           |           |            WAIT_CERT_CR
     *#           |           |        Recv |       | Recv CertificateRequest
     *#           |           | Certificate |       v
     *#           |           |             |    WAIT_CERT
     *#           |           |             |       | Recv Certificate
     *#           |           |             v       v
     *#           |           |              WAIT_CV
     *#           |           |                 | Recv CertificateVerify
     *#           |           +> WAIT_FINISHED <+
     *#           |                  | Recv Finished
     *#           \                  | [Send EndOfEarlyData]
     *#                              | K_send = handshake
     */
    if ((message_type == SERVER_FINISHED && !WITH_EARLY_DATA(conn))
            || (message_type == END_OF_EARLY_DATA)) {
        K_send(conn, S2N_HANDSHAKE_SECRET);
    }
    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#appendix-A.1
     *#                              | [Send Certificate [+ CertificateVerify]]
     *#    Can send                  | Send Finished
     *#    app data   -->            | K_send = K_recv = application
     */
    if (message_type == CLIENT_FINISHED) {
        K_send(conn, S2N_APPLICATION_SECRET);
        K_recv(conn, S2N_APPLICATION_SECRET);
    }
    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#appendix-A.1
     *#    after here                v
     *#                          CONNECTED
     */
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_server_key_schedule(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);

    message_type_t message_type = s2n_conn_get_current_message_type(conn);

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#appendix-A.2
     *#                              START <-----+
     *#               Recv ClientHello |         | Send HelloRetryRequest
     *#                                v         |
     *#                             RECVD_CH ----+
     *#                                | Select parameters
     *#                                v
     *#                             NEGOTIATED
     *#                                | Send ServerHello
     *#                                | K_send = handshake
     */
    if (message_type == SERVER_HELLO) {
        K_send(conn, S2N_HANDSHAKE_SECRET);
    }
    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#appendix-A.2
     *#                                | Send EncryptedExtensions
     *#                                | [Send CertificateRequest]
     *# Can send                       | [Send Certificate + CertificateVerify]
     *# app data                       | Send Finished
     *# after   -->                    | K_send = application
     */
    if (message_type == SERVER_FINISHED) {
        K_send(conn, S2N_APPLICATION_SECRET);
        /* clang-format off */
    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#appendix-A.2
     *# here                  +--------+--------+
     *#              No 0-RTT |                 | 0-RTT
     *#                       |                 |
     *#   K_recv = handshake  |                 | K_recv = early data
     */
        /* clang-format on */
        if (WITH_EARLY_DATA(conn)) {
            K_recv(conn, S2N_EARLY_SECRET);
        } else {
            K_recv(conn, S2N_HANDSHAKE_SECRET);
        }
    }
    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#appendix-A.2
     *# [Skip decrypt errors] |    +------> WAIT_EOED -+
     *#                       |    |       Recv |      | Recv EndOfEarlyData
     *#                       |    | early data |      | K_recv = handshake
     *#                       |    +------------+      |
     */
    if (message_type == END_OF_EARLY_DATA) {
        K_recv(conn, S2N_HANDSHAKE_SECRET);
    }
    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#appendix-A.2
     *#                       |                        |
     *#                       +> WAIT_FLIGHT2 <--------+
     *#                                |
     *#                       +--------+--------+
     *#               No auth |                 | Client auth
     *#                       |                 |
     *#                       |                 v
     *#                       |             WAIT_CERT
     *#                       |        Recv |       | Recv Certificate
     *#                       |       empty |       v
     *#                       | Certificate |    WAIT_CV
     *#                       |             |       | Recv
     *#                       |             v       | CertificateVerify
     *#                       +-> WAIT_FINISHED <---+
     *#                                | Recv Finished
     *#                                | K_recv = application
     */
    if (message_type == CLIENT_FINISHED) {
        K_recv(conn, S2N_APPLICATION_SECRET);
    }
    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#appendix-A.2
     *#                                v
     *#                            CONNECTED
     */
    return S2N_RESULT_OK;
}

s2n_result (*key_schedules[])(struct s2n_connection *) = {
    [S2N_CLIENT] = &s2n_client_key_schedule,
    [S2N_SERVER] = &s2n_server_key_schedule,
};

S2N_RESULT s2n_tls13_key_schedule_update(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);
    if (s2n_connection_get_protocol_version(conn) < S2N_TLS13) {
        return S2N_RESULT_OK;
    }
    RESULT_ENSURE_REF(key_schedules[conn->mode]);
    RESULT_GUARD(key_schedules[conn->mode](conn));
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_tls13_key_schedule_reset(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(conn->initial);
    conn->client = conn->initial;
    conn->server = conn->initial;
    conn->secrets.extract_secret_type = S2N_NONE_SECRET;
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_tls13_key_schedule_generate_key_material(struct s2n_connection *conn,
        s2n_mode sender, struct s2n_key_material *key_material)
{
    RESULT_GUARD(s2n_key_material_init(key_material, conn));
    if (sender == S2N_CLIENT) {
        RESULT_GUARD(s2n_tls13_key_schedule_get_keying_material(conn, S2N_MASTER_SECRET,
                sender, &key_material->client_iv, &key_material->client_key));
    } else {
        RESULT_GUARD(s2n_tls13_key_schedule_get_keying_material(conn, S2N_MASTER_SECRET,
                sender, &key_material->server_iv, &key_material->server_key));
    }
    return S2N_RESULT_OK;
}
