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

/**
 * This module implements key logging as defined by the NSS Key Log Format
 *
 * See https://developer.mozilla.org/en-US/docs/Mozilla/Projects/NSS/Key_Log_Format
 *
 * This key log file is a series of lines. Comment lines begin with a sharp
 * character ('#') and are ignored. Secrets follow the format
 * <Label> <space> <ClientRandom> <space> <Secret> where:
 *
 *   <Label> describes the following secret.
 *   <ClientRandom> is 32 bytes Random value from the Client Hello message, encoded as 64 hexadecimal characters.
 *   <Secret> depends on the Label (see below).
 *
 * The following labels are defined, followed by a description of the secret:
 *
 *   RSA: 48 bytes for the premaster secret, encoded as 96 hexadecimal characters (removed in NSS 3.34)
 *   CLIENT_RANDOM: 48 bytes for the master secret, encoded as 96 hexadecimal characters (for SSL 3.0, TLS 1.0, 1.1 and 1.2)
 *   CLIENT_EARLY_TRAFFIC_SECRET: the hex-encoded early traffic secret for the client side (for TLS 1.3)
 *   CLIENT_HANDSHAKE_TRAFFIC_SECRET: the hex-encoded handshake traffic secret for the client side (for TLS 1.3)
 *   SERVER_HANDSHAKE_TRAFFIC_SECRET: the hex-encoded handshake traffic secret for the server side (for TLS 1.3)
 *   CLIENT_TRAFFIC_SECRET_0: the first hex-encoded application traffic secret for the client side (for TLS 1.3)
 *   SERVER_TRAFFIC_SECRET_0: the first hex-encoded application traffic secret for the server side (for TLS 1.3)
 *   EARLY_EXPORTER_SECRET: the hex-encoded early exporter secret (for TLS 1.3).
 *   EXPORTER_SECRET: the hex-encoded exporter secret (for TLS 1.3)
 */

#include "api/s2n.h"
#include "tls/s2n_config.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_crypto_constants.h"
#include "tls/s2n_quic_support.h" /* this currently holds the s2n_secret_type_t enum */
#include "utils/s2n_blob.h"
#include "utils/s2n_safety.h"

/* hex requires 2 chars per byte */
#define HEX_ENCODING_SIZE 2

S2N_RESULT s2n_key_log_tls13_secret(struct s2n_connection *conn, const struct s2n_blob *secret, s2n_secret_type_t secret_type)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(conn->config);
    RESULT_ENSURE_REF(secret);

    /* only emit keys if the callback has been set */
    if (!conn->config->key_log_cb) {
        return S2N_RESULT_OK;
    }

    const uint8_t client_early_traffic_label[] = "CLIENT_EARLY_TRAFFIC_SECRET ";
    const uint8_t client_handshake_label[] = "CLIENT_HANDSHAKE_TRAFFIC_SECRET ";
    const uint8_t server_handshake_label[] = "SERVER_HANDSHAKE_TRAFFIC_SECRET ";
    const uint8_t client_traffic_label[] = "CLIENT_TRAFFIC_SECRET_0 ";
    const uint8_t server_traffic_label[] = "SERVER_TRAFFIC_SECRET_0 ";
    const uint8_t exporter_secret_label[] = "EXPORTER_SECRET ";

    const uint8_t *label = NULL;
    uint8_t label_size = 0;

    switch (secret_type) {
        case S2N_CLIENT_EARLY_TRAFFIC_SECRET:
            label = client_early_traffic_label;
            label_size = sizeof(client_early_traffic_label) - 1;
            break;
        case S2N_CLIENT_HANDSHAKE_TRAFFIC_SECRET:
            label = client_handshake_label;
            label_size = sizeof(client_handshake_label) - 1;
            break;
        case S2N_SERVER_HANDSHAKE_TRAFFIC_SECRET:
            label = server_handshake_label;
            label_size = sizeof(server_handshake_label) - 1;
            break;
        case S2N_CLIENT_APPLICATION_TRAFFIC_SECRET:
            label = client_traffic_label;
            label_size = sizeof(client_traffic_label) - 1;
            break;
        case S2N_SERVER_APPLICATION_TRAFFIC_SECRET:
            label = server_traffic_label;
            label_size = sizeof(server_traffic_label) - 1;
            break;
        case S2N_EXPORTER_SECRET:
            label = exporter_secret_label;
            label_size = sizeof(exporter_secret_label) - 1;
            break;
        default:
            /* Ignore the secret types we don't understand */
            return S2N_RESULT_OK;
    }

    const uint8_t len = label_size
            + S2N_TLS_RANDOM_DATA_LEN * HEX_ENCODING_SIZE
            + 1 /* SPACE */
            + secret->size * HEX_ENCODING_SIZE;

    DEFER_CLEANUP(struct s2n_stuffer output, s2n_stuffer_free);
    RESULT_GUARD_POSIX(s2n_stuffer_alloc(&output, len));

    struct s2n_blob client_random = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&client_random, conn->handshake_params.client_random,
            sizeof(conn->handshake_params.client_random)));

    RESULT_GUARD_POSIX(s2n_stuffer_write_bytes(&output, label, label_size));
    RESULT_GUARD(s2n_stuffer_write_hex(&output, &client_random));
    RESULT_GUARD_POSIX(s2n_stuffer_write_uint8(&output, ' '));
    RESULT_GUARD(s2n_stuffer_write_hex(&output, secret));

    uint8_t *data = s2n_stuffer_raw_read(&output, len);
    RESULT_ENSURE_REF(data);

    conn->config->key_log_cb(conn->config->key_log_ctx, conn, data, len);

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_key_log_tls12_secret(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(conn->config);

    /* only emit keys if the callback has been set */
    if (!conn->config->key_log_cb) {
        return S2N_RESULT_OK;
    }

    /* CLIENT_RANDOM: 48 bytes for the master secret, encoded as 96 hexadecimal characters (for SSL 3.0, TLS 1.0, 1.1 and 1.2) */
    const uint8_t label[] = "CLIENT_RANDOM ";
    const uint8_t label_size = sizeof(label) - 1;

    const uint8_t len = label_size
            + S2N_TLS_RANDOM_DATA_LEN * HEX_ENCODING_SIZE
            + 1 /* SPACE */
            + S2N_TLS_SECRET_LEN * HEX_ENCODING_SIZE;

    DEFER_CLEANUP(struct s2n_stuffer output, s2n_stuffer_free);
    RESULT_GUARD_POSIX(s2n_stuffer_alloc(&output, len));

    struct s2n_blob client_random = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&client_random, conn->handshake_params.client_random,
            sizeof(conn->handshake_params.client_random)));

    struct s2n_blob master_secret = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&master_secret, conn->secrets.version.tls12.master_secret,
            sizeof(conn->secrets.version.tls12.master_secret)));

    RESULT_GUARD_POSIX(s2n_stuffer_write_bytes(&output, label, label_size));
    RESULT_GUARD(s2n_stuffer_write_hex(&output, &client_random));
    RESULT_GUARD_POSIX(s2n_stuffer_write_uint8(&output, ' '));
    RESULT_GUARD(s2n_stuffer_write_hex(&output, &master_secret));

    uint8_t *data = s2n_stuffer_raw_read(&output, len);
    RESULT_ENSURE_REF(data);

    conn->config->key_log_cb(conn->config->key_log_ctx, conn, data, len);

    return S2N_RESULT_OK;
}
