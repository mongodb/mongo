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

#include "api/s2n.h"

/*
 * APIs intended to support an external implementation of the QUIC protocol:
 * https://datatracker.ietf.org/wg/quic/about/
 *
 * QUIC requires access to parts of S2N not usually surfaced to customers. These APIs change
 * the behavior of S2N in potentially dangerous ways and should only be used by implementations
 * of the QUIC protocol.
 *
 * Additionally, all QUIC APIs are considered experimental and are subject to change without
 * notice. They should only be used for testing purposes.
 */

S2N_API int s2n_config_enable_quic(struct s2n_config *config);
S2N_API int s2n_connection_enable_quic(struct s2n_connection *conn);
S2N_API bool s2n_connection_is_quic_enabled(struct s2n_connection *conn);
S2N_API bool s2n_connection_are_session_tickets_enabled(struct s2n_connection *conn);

/*
 * Set the data to be sent in the quic_transport_parameters extension.
 * The data provided will be copied into a buffer owned by S2N.
 */
S2N_API int s2n_connection_set_quic_transport_parameters(struct s2n_connection *conn,
        const uint8_t *data_buffer, uint16_t data_len);

/*
 * Retrieve the data from the peer's quic_transport_parameters extension.
 * data_buffer will be set to a buffer owned by S2N which will be freed when the connection is freed.
 * data_len will be set to the length of the data returned.
 *
 * S2N treats the extension data as opaque bytes and performs no validation.
 */
S2N_API int s2n_connection_get_quic_transport_parameters(struct s2n_connection *conn,
        const uint8_t **data_buffer, uint16_t *data_len);

typedef enum {
    S2N_CLIENT_EARLY_TRAFFIC_SECRET = 0,
    S2N_CLIENT_HANDSHAKE_TRAFFIC_SECRET,
    S2N_SERVER_HANDSHAKE_TRAFFIC_SECRET,
    S2N_CLIENT_APPLICATION_TRAFFIC_SECRET,
    S2N_SERVER_APPLICATION_TRAFFIC_SECRET,
    S2N_EXPORTER_SECRET,
} s2n_secret_type_t;

/*
 * Called when S2N begins using a new key.
 *
 * The memory pointed to by "secret" will be wiped after this method returns and should be copied by
 * the application if necessary. The application should also be very careful managing the memory and
 * lifespan of the secret: if the secret is compromised, TLS is compromised.
 */
typedef int (*s2n_secret_cb)(void *context, struct s2n_connection *conn,
        s2n_secret_type_t secret_type, uint8_t *secret, uint8_t secret_size);

/*
 * Set the function to be called when S2N begins using a new key.
 *
 * The callback function will ONLY be triggered if QUIC is enabled. This API is not intended to be
 * used outside of a QUIC implementation.
 */
S2N_API int s2n_connection_set_secret_callback(struct s2n_connection *conn, s2n_secret_cb cb_func, void *ctx);

/*
 * Return the TLS alert that S2N-TLS would send, if S2N-TLS sent specific alerts.
 *
 * S2N-TLS only sends generic close_notify alerts for security reasons, and TLS never
 * sends alerts when used by QUIC. This method returns the alert that would have been
 * sent if S2N-TLS sent specific alerts as defined in the protocol specifications.
 *
 * WARNING: this method is still considered experimental and will not always report
 * the correct alert description. It may be used for testing and logging, but
 * not relied on for production logic.
 */
S2N_API int s2n_error_get_alert(int error, uint8_t *alert);

/* Attempts to read and process a post-handshake message from QUIC. This function
 * should be called when post-handshake messages in QUIC have been received.
 */
S2N_API int s2n_recv_quic_post_handshake_message(struct s2n_connection *conn, s2n_blocked_status *blocked);
