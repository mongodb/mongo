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

#include "tls/extensions/s2n_psk_key_exchange_modes.h"

#include <stdint.h>
#include <sys/param.h>

#include "tls/extensions/s2n_client_psk.h"
#include "tls/s2n_tls_parameters.h"
#include "utils/s2n_safety.h"

static bool s2n_psk_key_exchange_modes_should_send(struct s2n_connection *conn);
static int s2n_psk_key_exchange_modes_send(struct s2n_connection *conn, struct s2n_stuffer *out);
static int s2n_psk_key_exchange_modes_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);

const s2n_extension_type s2n_psk_key_exchange_modes_extension = {
    .iana_value = TLS_EXTENSION_PSK_KEY_EXCHANGE_MODES,
    .minimum_version = S2N_TLS13,
    .is_response = false,
    .send = s2n_psk_key_exchange_modes_send,
    .recv = s2n_psk_key_exchange_modes_recv,
    .should_send = s2n_psk_key_exchange_modes_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};

static bool s2n_psk_key_exchange_modes_should_send(struct s2n_connection *conn)
{
    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.9
     *# Servers MUST NOT select a key exchange mode that is not listed by the
     *# client.  This extension also restricts the modes for use with PSK
     *# resumption.
     *
     * The RFC is ambiguous about whether the psk_kx_modes extension should be
     * sent in the first flight of messages, but we choose to do so for
     * interoperability with other TLS implementations.
     * https://github.com/aws/s2n-tls/issues/4124
     * The final check for psk_list.len > 0 is necessary because external
     * PSKs are used without setting `use_tickets` to true.
     */
    return conn->config->use_tickets || conn->psk_params.psk_list.len > 0;
}

static int s2n_psk_key_exchange_modes_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(conn);

    POSIX_GUARD(s2n_stuffer_write_uint8(out, PSK_KEY_EXCHANGE_MODE_SIZE));

    /* s2n currently only supports pre-shared keys with (EC)DHE key establishment */
    POSIX_GUARD(s2n_stuffer_write_uint8(out, TLS_PSK_DHE_KE_MODE));

    return S2N_SUCCESS;
}

static int s2n_psk_key_exchange_modes_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    POSIX_ENSURE_REF(conn);

    uint8_t psk_ke_mode_list_len = 0;
    POSIX_GUARD(s2n_stuffer_read_uint8(extension, &psk_ke_mode_list_len));
    if (psk_ke_mode_list_len > s2n_stuffer_data_available(extension)) {
        /* Malformed length, ignore the extension */
        return S2N_SUCCESS;
    }

    for (size_t i = 0; i < psk_ke_mode_list_len; i++) {
        uint8_t wire_psk_ke_mode = 0;
        POSIX_GUARD(s2n_stuffer_read_uint8(extension, &wire_psk_ke_mode));

        /* s2n currently only supports pre-shared keys with (EC)DHE key establishment */
        if (wire_psk_ke_mode == TLS_PSK_DHE_KE_MODE) {
            conn->psk_params.psk_ke_mode = S2N_PSK_DHE_KE;
            return S2N_SUCCESS;
        }
    }

    return S2N_SUCCESS;
}
