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

#include "tls/s2n_early_data.h"

#include <sys/param.h>

#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_psk.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_safety.h"

const s2n_early_data_state valid_previous_states[] = {
    [S2N_EARLY_DATA_REQUESTED] = S2N_UNKNOWN_EARLY_DATA_STATE,
    [S2N_EARLY_DATA_NOT_REQUESTED] = S2N_UNKNOWN_EARLY_DATA_STATE,
    [S2N_EARLY_DATA_REJECTED] = S2N_EARLY_DATA_REQUESTED,
    [S2N_EARLY_DATA_ACCEPTED] = S2N_EARLY_DATA_REQUESTED,
    [S2N_END_OF_EARLY_DATA] = S2N_EARLY_DATA_ACCEPTED,
};

S2N_RESULT s2n_connection_set_early_data_state(struct s2n_connection *conn, s2n_early_data_state next_state)
{
    RESULT_ENSURE_REF(conn);
    if (conn->early_data_state == next_state) {
        return S2N_RESULT_OK;
    }
    RESULT_ENSURE(next_state < S2N_EARLY_DATA_STATES_COUNT, S2N_ERR_INVALID_EARLY_DATA_STATE);
    RESULT_ENSURE(next_state != S2N_UNKNOWN_EARLY_DATA_STATE, S2N_ERR_INVALID_EARLY_DATA_STATE);
    RESULT_ENSURE(conn->early_data_state == valid_previous_states[next_state], S2N_ERR_INVALID_EARLY_DATA_STATE);
    conn->early_data_state = next_state;
    return S2N_RESULT_OK;
}

int s2n_connection_set_early_data_expected(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    conn->early_data_expected = true;
    return S2N_SUCCESS;
}

int s2n_connection_set_end_of_early_data(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    conn->early_data_expected = false;
    return S2N_SUCCESS;
}

static S2N_RESULT s2n_early_data_validate(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(conn->secure);

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.10
     *# In order to accept early data, the server MUST have accepted a PSK
     *# cipher suite and selected the first key offered in the client's
     *# "pre_shared_key" extension.
     **/
    RESULT_ENSURE_REF(conn->psk_params.chosen_psk);
    RESULT_ENSURE_EQ(conn->psk_params.chosen_psk_wire_index, 0);

    struct s2n_early_data_config *config = &conn->psk_params.chosen_psk->early_data_config;
    RESULT_ENSURE_GT(config->max_early_data_size, 0);

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.10
     *# In addition, it MUST verify that the
     *# following values are the same as those associated with the
     *# selected PSK:
     *#
     *# -  The TLS version number
     **/
    RESULT_ENSURE_EQ(config->protocol_version, s2n_connection_get_protocol_version(conn));
    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.10
     *# -  The selected cipher suite
     **/
    RESULT_ENSURE_EQ(config->cipher_suite, conn->secure->cipher_suite);
    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.10
     *# -  The selected ALPN [RFC7301] protocol, if any
     **/
    const size_t app_protocol_size = strlen(conn->application_protocol);
    if (app_protocol_size > 0 || config->application_protocol.size > 0) {
        RESULT_ENSURE_EQ(config->application_protocol.size, app_protocol_size + 1 /* null-terminating char */);
        RESULT_ENSURE(s2n_constant_time_equals(config->application_protocol.data, (uint8_t *) conn->application_protocol, app_protocol_size), S2N_ERR_SAFETY);
    }

    return S2N_RESULT_OK;
}

bool s2n_early_data_is_valid_for_connection(struct s2n_connection *conn)
{
    return s2n_result_is_ok(s2n_early_data_validate(conn));
}

S2N_RESULT s2n_early_data_accept_or_reject(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);
    if (conn->early_data_state != S2N_EARLY_DATA_REQUESTED) {
        return S2N_RESULT_OK;
    }

    if (conn->handshake.early_data_async_state.conn) {
        RESULT_BAIL(S2N_ERR_ASYNC_BLOCKED);
    }

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.10
     *# If any of these checks fail, the server MUST NOT respond with the
     *# extension
     **/
    if (!s2n_early_data_is_valid_for_connection(conn)) {
        RESULT_GUARD(s2n_connection_set_early_data_state(conn, S2N_EARLY_DATA_REJECTED));
        return S2N_RESULT_OK;
    }

    /* Even if the connection is valid for early data, the client can't consider
     * early data accepted until the server sends the early data indication. */
    if (conn->mode == S2N_CLIENT) {
        return S2N_RESULT_OK;
    }

    /* The server should reject early data if the application is not prepared to handle it. */
    if (!conn->early_data_expected) {
        RESULT_GUARD(s2n_connection_set_early_data_state(conn, S2N_EARLY_DATA_REJECTED));
        return S2N_RESULT_OK;
    }

    /* If early data would otherwise be accepted, let the application apply any additional restrictions.
     * For example, an application could use this callback to implement anti-replay protections.
     *
     * This callback can be either synchronous or asynchronous. The handshake will not proceed until
     * the application either accepts or rejects early data.
     */
    RESULT_ENSURE_REF(conn->config);
    if (conn->config->early_data_cb) {
        conn->handshake.early_data_async_state.conn = conn;
        RESULT_ENSURE(conn->config->early_data_cb(conn, &conn->handshake.early_data_async_state) >= S2N_SUCCESS,
                S2N_ERR_CANCELLED);
        if (conn->early_data_state == S2N_EARLY_DATA_REQUESTED) {
            RESULT_BAIL(S2N_ERR_ASYNC_BLOCKED);
        }
    } else {
        RESULT_GUARD(s2n_connection_set_early_data_state(conn, S2N_EARLY_DATA_ACCEPTED));
    }
    return S2N_RESULT_OK;
}

int s2n_config_set_server_max_early_data_size(struct s2n_config *config, uint32_t max_early_data_size)
{
    POSIX_ENSURE_REF(config);
    config->server_max_early_data_size = max_early_data_size;
    return S2N_SUCCESS;
}

int s2n_connection_set_server_max_early_data_size(struct s2n_connection *conn, uint32_t max_early_data_size)
{
    POSIX_ENSURE_REF(conn);
    conn->server_max_early_data_size = max_early_data_size;
    conn->server_max_early_data_size_overridden = true;
    return S2N_SUCCESS;
}

S2N_RESULT s2n_early_data_get_server_max_size(struct s2n_connection *conn, uint32_t *max_early_data_size)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(max_early_data_size);
    if (conn->server_max_early_data_size_overridden) {
        *max_early_data_size = conn->server_max_early_data_size;
    } else {
        RESULT_ENSURE_REF(conn->config);
        *max_early_data_size = conn->config->server_max_early_data_size;
    }
    return S2N_RESULT_OK;
}

int s2n_connection_set_server_early_data_context(struct s2n_connection *conn, const uint8_t *context, uint16_t context_size)
{
    POSIX_ENSURE_REF(conn);
    if (context_size > 0) {
        POSIX_ENSURE_REF(context);
    }

    POSIX_GUARD(s2n_realloc(&conn->server_early_data_context, context_size));
    POSIX_CHECKED_MEMCPY(conn->server_early_data_context.data, context, context_size);
    return S2N_SUCCESS;
}

S2N_CLEANUP_RESULT s2n_early_data_config_free(struct s2n_early_data_config *config)
{
    if (config == NULL) {
        return S2N_RESULT_OK;
    }
    RESULT_GUARD_POSIX(s2n_free(&config->application_protocol));
    RESULT_GUARD_POSIX(s2n_free(&config->context));
    return S2N_RESULT_OK;
}

int s2n_psk_configure_early_data(struct s2n_psk *psk, uint32_t max_early_data_size,
        uint8_t cipher_suite_first_byte, uint8_t cipher_suite_second_byte)
{
    POSIX_ENSURE_REF(psk);

    const uint8_t cipher_suite_iana[] = { cipher_suite_first_byte, cipher_suite_second_byte };
    struct s2n_cipher_suite *cipher_suite = NULL;
    POSIX_GUARD_RESULT(s2n_cipher_suite_from_iana(cipher_suite_iana, sizeof(cipher_suite_iana), &cipher_suite));
    POSIX_ENSURE_REF(cipher_suite);
    POSIX_ENSURE(cipher_suite->prf_alg == psk->hmac_alg, S2N_ERR_INVALID_ARGUMENT);

    psk->early_data_config.max_early_data_size = max_early_data_size;
    psk->early_data_config.protocol_version = S2N_TLS13;
    psk->early_data_config.cipher_suite = cipher_suite;
    return S2N_SUCCESS;
}

int s2n_psk_set_application_protocol(struct s2n_psk *psk, const uint8_t *application_protocol, uint8_t size)
{
    POSIX_ENSURE_REF(psk);
    if (size > 0) {
        POSIX_ENSURE_REF(application_protocol);
    }
    struct s2n_blob *protocol_blob = &psk->early_data_config.application_protocol;
    POSIX_GUARD(s2n_realloc(protocol_blob, size));
    POSIX_CHECKED_MEMCPY(protocol_blob->data, application_protocol, size);
    return S2N_SUCCESS;
}

int s2n_psk_set_early_data_context(struct s2n_psk *psk, const uint8_t *context, uint16_t size)
{
    POSIX_ENSURE_REF(psk);
    if (size > 0) {
        POSIX_ENSURE_REF(context);
    }
    struct s2n_blob *context_blob = &psk->early_data_config.context;
    POSIX_GUARD(s2n_realloc(context_blob, size));
    POSIX_CHECKED_MEMCPY(context_blob->data, context, size);
    return S2N_SUCCESS;
}

S2N_RESULT s2n_early_data_config_clone(struct s2n_psk *new_psk, struct s2n_early_data_config *old_config)
{
    RESULT_ENSURE_REF(old_config);
    RESULT_ENSURE_REF(new_psk);

    struct s2n_early_data_config config_copy = new_psk->early_data_config;

    /* Copy all fields from the old_config EXCEPT the blobs, which we need to reallocate. */
    new_psk->early_data_config = *old_config;
    new_psk->early_data_config.application_protocol = config_copy.application_protocol;
    new_psk->early_data_config.context = config_copy.context;

    /* Clone / realloc blobs */
    RESULT_GUARD_POSIX(s2n_psk_set_application_protocol(new_psk, old_config->application_protocol.data,
            old_config->application_protocol.size));
    RESULT_GUARD_POSIX(s2n_psk_set_early_data_context(new_psk, old_config->context.data,
            old_config->context.size));

    return S2N_RESULT_OK;
}

int s2n_connection_get_early_data_status(struct s2n_connection *conn, s2n_early_data_status_t *status)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(status);

    switch (conn->early_data_state) {
        case S2N_EARLY_DATA_STATES_COUNT:
            break;
        case S2N_EARLY_DATA_NOT_REQUESTED:
            *status = S2N_EARLY_DATA_STATUS_NOT_REQUESTED;
            return S2N_SUCCESS;
        case S2N_EARLY_DATA_REJECTED:
            *status = S2N_EARLY_DATA_STATUS_REJECTED;
            return S2N_SUCCESS;
        case S2N_END_OF_EARLY_DATA:
            *status = S2N_EARLY_DATA_STATUS_END;
            return S2N_SUCCESS;
        case S2N_UNKNOWN_EARLY_DATA_STATE:
        case S2N_EARLY_DATA_REQUESTED:
        case S2N_EARLY_DATA_ACCEPTED:
            *status = S2N_EARLY_DATA_STATUS_OK;
            return S2N_SUCCESS;
    }
    POSIX_BAIL(S2N_ERR_INVALID_EARLY_DATA_STATE);
}

static S2N_RESULT s2n_get_remaining_early_data_bytes(struct s2n_connection *conn, uint32_t *early_data_allowed)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(early_data_allowed);
    *early_data_allowed = 0;

    uint32_t max_early_data_size = 0;
    RESULT_GUARD_POSIX(s2n_connection_get_max_early_data_size(conn, &max_early_data_size));

    RESULT_ENSURE(max_early_data_size >= conn->early_data_bytes, S2N_ERR_MAX_EARLY_DATA_SIZE);
    *early_data_allowed = (max_early_data_size - conn->early_data_bytes);

    return S2N_RESULT_OK;
}

int s2n_connection_get_remaining_early_data_size(struct s2n_connection *conn, uint32_t *allowed_early_data_size)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(allowed_early_data_size);
    *allowed_early_data_size = 0;

    switch (conn->early_data_state) {
        case S2N_EARLY_DATA_STATES_COUNT:
        case S2N_EARLY_DATA_NOT_REQUESTED:
        case S2N_EARLY_DATA_REJECTED:
        case S2N_END_OF_EARLY_DATA:
            *allowed_early_data_size = 0;
            break;
        case S2N_UNKNOWN_EARLY_DATA_STATE:
        case S2N_EARLY_DATA_REQUESTED:
        case S2N_EARLY_DATA_ACCEPTED:
            POSIX_GUARD_RESULT(s2n_get_remaining_early_data_bytes(conn, allowed_early_data_size));
            break;
    }
    return S2N_SUCCESS;
}

int s2n_connection_get_max_early_data_size(struct s2n_connection *conn, uint32_t *max_early_data_size)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(max_early_data_size);
    *max_early_data_size = 0;

    uint32_t server_max_early_data_size = 0;
    POSIX_GUARD_RESULT(s2n_early_data_get_server_max_size(conn, &server_max_early_data_size));

    if (conn->psk_params.psk_list.len == 0) {
        /* This method may be called by the server before loading its PSKs.
         * The server can load its PSKs during the handshake, either via the PSK selection callback
         * or by receiving a stateless session ticket.
         *
         * Before that happens, we should make an optimistic assumption of the early data size.
         * That way, the max early data size always decreases (for example, it won't go from 0 -> UINT32_MAX
         * after receiving a PSK in the ClientHello).
         */
        if (conn->mode == S2N_SERVER && !IS_NEGOTIATED(conn)) {
            *max_early_data_size = server_max_early_data_size;
        }
        return S2N_SUCCESS;
    }

    struct s2n_psk *first_psk = NULL;
    POSIX_GUARD_RESULT(s2n_array_get(&conn->psk_params.psk_list, 0, (void **) &first_psk));
    POSIX_ENSURE_REF(first_psk);
    *max_early_data_size = first_psk->early_data_config.max_early_data_size;

    /* For the server, we should use the minimum of the limit retrieved from the ticket
     * and the current limit being set for new tickets.
     *
     * This is defensive: even if more early data was previously allowed, the server may not be
     * willing or able to handle that much early data now.
     *
     * We don't do this for external PSKs because the server has intentionally set the limit
     * while setting up this connection, not during a previous connection.
     */
    if (conn->mode == S2N_SERVER && first_psk->type == S2N_PSK_TYPE_RESUMPTION) {
        *max_early_data_size = MIN(*max_early_data_size, server_max_early_data_size);
    }

    return S2N_SUCCESS;
}

int s2n_config_set_early_data_cb(struct s2n_config *config, s2n_early_data_cb cb)
{
    POSIX_ENSURE_REF(config);
    config->early_data_cb = cb;
    return S2N_SUCCESS;
}

int s2n_offered_early_data_get_context_length(struct s2n_offered_early_data *early_data, uint16_t *context_len)
{
    POSIX_ENSURE_REF(context_len);
    POSIX_ENSURE_REF(early_data);
    struct s2n_connection *conn = early_data->conn;

    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->psk_params.chosen_psk);
    struct s2n_early_data_config *early_data_config = &conn->psk_params.chosen_psk->early_data_config;

    *context_len = early_data_config->context.size;

    return S2N_SUCCESS;
}

int s2n_offered_early_data_get_context(struct s2n_offered_early_data *early_data, uint8_t *context, uint16_t max_len)
{
    POSIX_ENSURE_REF(context);
    POSIX_ENSURE_REF(early_data);
    struct s2n_connection *conn = early_data->conn;

    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->psk_params.chosen_psk);
    struct s2n_early_data_config *early_data_config = &conn->psk_params.chosen_psk->early_data_config;

    POSIX_ENSURE(early_data_config->context.size <= max_len, S2N_ERR_INSUFFICIENT_MEM_SIZE);
    POSIX_CHECKED_MEMCPY(context, early_data_config->context.data, early_data_config->context.size);

    return S2N_SUCCESS;
}

int s2n_offered_early_data_reject(struct s2n_offered_early_data *early_data)
{
    POSIX_ENSURE_REF(early_data);
    struct s2n_connection *conn = early_data->conn;
    POSIX_ENSURE_REF(conn);
    POSIX_GUARD_RESULT(s2n_connection_set_early_data_state(conn, S2N_EARLY_DATA_REJECTED));
    return S2N_SUCCESS;
}

int s2n_offered_early_data_accept(struct s2n_offered_early_data *early_data)
{
    POSIX_ENSURE_REF(early_data);
    struct s2n_connection *conn = early_data->conn;
    POSIX_ENSURE_REF(conn);
    POSIX_GUARD_RESULT(s2n_connection_set_early_data_state(conn, S2N_EARLY_DATA_ACCEPTED));
    return S2N_SUCCESS;
}
