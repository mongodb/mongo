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
#include "tls/s2n_connection.h"
#include "utils/s2n_safety.h"

S2N_RESULT s2n_protocol_preferences_read(struct s2n_stuffer *protocol_preferences, struct s2n_blob *protocol)
{
    RESULT_ENSURE_REF(protocol_preferences);
    RESULT_ENSURE_REF(protocol);

    uint8_t length = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint8(protocol_preferences, &length));
    RESULT_ENSURE_GT(length, 0);

    uint8_t *data = s2n_stuffer_raw_read(protocol_preferences, length);
    RESULT_ENSURE_REF(data);

    RESULT_GUARD_POSIX(s2n_blob_init(protocol, data, length));
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_protocol_preferences_contain(struct s2n_blob *protocol_preferences, struct s2n_blob *protocol, bool *contains)
{
    RESULT_ENSURE_REF(contains);
    *contains = false;
    RESULT_ENSURE_REF(protocol_preferences);
    RESULT_ENSURE_REF(protocol);

    struct s2n_stuffer app_protocols_stuffer = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init(&app_protocols_stuffer, protocol_preferences));
    RESULT_GUARD_POSIX(s2n_stuffer_skip_write(&app_protocols_stuffer, protocol_preferences->size));

    while (s2n_stuffer_data_available(&app_protocols_stuffer) > 0) {
        struct s2n_blob match_against = { 0 };
        RESULT_GUARD(s2n_protocol_preferences_read(&app_protocols_stuffer, &match_against));

        if (match_against.size == protocol->size && memcmp(match_against.data, protocol->data, protocol->size) == 0) {
            *contains = true;
            return S2N_RESULT_OK;
        }
    }
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_protocol_preferences_append(struct s2n_blob *application_protocols, const uint8_t *protocol, uint8_t protocol_len)
{
    RESULT_ENSURE_MUT(application_protocols);
    RESULT_ENSURE_REF(protocol);

    /**
     *= https://www.rfc-editor.org/rfc/rfc7301#section-3.1
     *# Empty strings
     *# MUST NOT be included and byte strings MUST NOT be truncated.
     */
    RESULT_ENSURE(protocol_len != 0, S2N_ERR_INVALID_APPLICATION_PROTOCOL);

    uint32_t prev_len = application_protocols->size;
    uint32_t new_len = prev_len + /* len prefix */ 1 + protocol_len;
    RESULT_ENSURE(new_len <= UINT16_MAX, S2N_ERR_INVALID_APPLICATION_PROTOCOL);

    RESULT_GUARD_POSIX(s2n_realloc(application_protocols, new_len));

    struct s2n_stuffer protocol_stuffer = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init(&protocol_stuffer, application_protocols));
    RESULT_GUARD_POSIX(s2n_stuffer_skip_write(&protocol_stuffer, prev_len));
    RESULT_GUARD_POSIX(s2n_stuffer_write_uint8(&protocol_stuffer, protocol_len));
    RESULT_GUARD_POSIX(s2n_stuffer_write_bytes(&protocol_stuffer, protocol, protocol_len));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_protocol_preferences_set(struct s2n_blob *application_protocols, const char *const *protocols, int protocol_count)
{
    RESULT_ENSURE_MUT(application_protocols);

    /* NULL value indicates no preference so free the previous blob */
    if (protocols == NULL || protocol_count == 0) {
        RESULT_GUARD_POSIX(s2n_free(application_protocols));
        return S2N_RESULT_OK;
    }

    DEFER_CLEANUP(struct s2n_blob new_protocols = { 0 }, s2n_free);

    /* Allocate enough space to avoid a reallocation for every entry
     *
     * We assume that each protocol is most likely 8 bytes or less.
     * If it ends up being larger, we will expand the blob automatically
     * in the append method.
     */
    RESULT_GUARD_POSIX(s2n_realloc(&new_protocols, protocol_count * 8));

    /* set the size back to 0 so we start at the beginning.
     * s2n_realloc will just update the size field here
     */
    RESULT_GUARD_POSIX(s2n_realloc(&new_protocols, 0));
    RESULT_ENSURE_GTE(protocol_count, 0);

    for (size_t i = 0; i < (size_t) protocol_count; i++) {
        const uint8_t *protocol = (const uint8_t *) protocols[i];
        size_t length = strlen(protocols[i]);

        /**
         *= https://www.rfc-editor.org/rfc/rfc7301#section-3.1
         *# Empty strings
         *# MUST NOT be included and byte strings MUST NOT be truncated.
         */
        RESULT_ENSURE(length < 256, S2N_ERR_INVALID_APPLICATION_PROTOCOL);

        RESULT_GUARD(s2n_protocol_preferences_append(&new_protocols, protocol, (uint8_t) length));
    }

    /* now we can free the previous list since we've validated all new input */
    RESULT_GUARD_POSIX(s2n_free(application_protocols));

    /* update the connection/config application_protocols with the newly allocated blob */
    *application_protocols = new_protocols;

    ZERO_TO_DISABLE_DEFER_CLEANUP(new_protocols);

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_select_server_preference_protocol(struct s2n_connection *conn, struct s2n_stuffer *server_list,
        struct s2n_blob *client_list)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(server_list);
    RESULT_ENSURE_REF(client_list);

    while (s2n_stuffer_data_available(server_list) > 0) {
        struct s2n_blob protocol = { 0 };
        RESULT_ENSURE_OK(s2n_protocol_preferences_read(server_list, &protocol), S2N_ERR_BAD_MESSAGE);

        bool match_found = false;
        RESULT_ENSURE_OK(s2n_protocol_preferences_contain(client_list, &protocol, &match_found), S2N_ERR_BAD_MESSAGE);

        if (match_found) {
            RESULT_ENSURE_LT(protocol.size, sizeof(conn->application_protocol));
            RESULT_CHECKED_MEMCPY(conn->application_protocol, protocol.data, protocol.size);
            conn->application_protocol[protocol.size] = '\0';
            return S2N_RESULT_OK;
        }
    }

    return S2N_RESULT_OK;
}

int s2n_config_set_protocol_preferences(struct s2n_config *config, const char *const *protocols, int protocol_count)
{
    POSIX_GUARD_RESULT(s2n_protocol_preferences_set(&config->application_protocols, protocols, protocol_count));
    return S2N_SUCCESS;
}

int s2n_config_append_protocol_preference(struct s2n_config *config, const uint8_t *protocol, uint8_t protocol_len)
{
    POSIX_GUARD_RESULT(s2n_protocol_preferences_append(&config->application_protocols, protocol, protocol_len));
    return S2N_SUCCESS;
}

int s2n_connection_set_protocol_preferences(struct s2n_connection *conn, const char *const *protocols, int protocol_count)
{
    POSIX_GUARD_RESULT(s2n_protocol_preferences_set(&conn->application_protocols_overridden, protocols, protocol_count));
    return S2N_SUCCESS;
}

int s2n_connection_append_protocol_preference(struct s2n_connection *conn, const uint8_t *protocol, uint8_t protocol_len)
{
    POSIX_GUARD_RESULT(s2n_protocol_preferences_append(&conn->application_protocols_overridden, protocol, protocol_len));
    return S2N_SUCCESS;
}
