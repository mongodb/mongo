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

#include "tls/s2n_ktls.h"

#include "crypto/s2n_ktls_crypto.h"
#include "tls/s2n_prf.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls13_key_schedule.h"

/* Used for overriding setsockopt calls in testing */
s2n_setsockopt_fn s2n_setsockopt = setsockopt;

S2N_RESULT s2n_ktls_set_setsockopt_cb(s2n_setsockopt_fn cb)
{
    RESULT_ENSURE(s2n_in_test(), S2N_ERR_NOT_IN_TEST);
    s2n_setsockopt = cb;
    return S2N_RESULT_OK;
}

bool s2n_ktls_is_supported_on_platform()
{
#if defined(S2N_KTLS_SUPPORTED)
    return true;
#else
    return false;
#endif
}

static int s2n_ktls_disabled_read(void *io_context, uint8_t *buf, uint32_t len)
{
    POSIX_BAIL(S2N_ERR_IO);
}

static S2N_RESULT s2n_ktls_validate(struct s2n_connection *conn, s2n_ktls_mode ktls_mode)
{
    RESULT_ENSURE_REF(conn);
    const struct s2n_config *config = conn->config;
    RESULT_ENSURE_REF(config);

    RESULT_ENSURE(s2n_ktls_is_supported_on_platform(), S2N_ERR_KTLS_UNSUPPORTED_PLATFORM);

    /* kTLS enable should only be called once the handshake has completed. */
    RESULT_ENSURE(is_handshake_complete(conn), S2N_ERR_HANDSHAKE_NOT_COMPLETE);

    /* kTLS uses the prf_space to recalculate the keys, but the prf_space may be
     * freed by s2n_connection_free_handshake to reduce the connection size.
     * Explicitly check for prf_space here to avoid a confusing S2N_ERR_NULL later.
     */
    RESULT_ENSURE(conn->prf_space, S2N_ERR_INVALID_STATE);

    /* For now, only allow TlS1.3 if explicitly enabled.
     *
     * TLS1.3 is potentially more dangerous to enable than TLS1.2, since the kernel
     * does not currently support updating TLS keys and therefore will fail if
     * KeyUpdate messages are encountered.
     */
    bool version_supported = (conn->actual_protocol_version == S2N_TLS12)
            || (conn->config->ktls_tls13_enabled && conn->actual_protocol_version == S2N_TLS13);
    RESULT_ENSURE(version_supported, S2N_ERR_KTLS_UNSUPPORTED_CONN);

    /* Check if the cipher supports kTLS */
    const struct s2n_cipher *cipher = NULL;
    RESULT_GUARD(s2n_connection_get_secure_cipher(conn, &cipher));
    RESULT_ENSURE_REF(cipher);
    RESULT_ENSURE(cipher->set_ktls_info, S2N_ERR_KTLS_UNSUPPORTED_CONN);

    /* Renegotiation requires updating the keys, which kTLS doesn't currently support.
     *
     * Setting the renegotiation callback doesn't guarantee that a client will
     * attempt to renegotiate. The callback can also be used to send warning alerts
     * signaling that renegotiation was rejected. However, we can provide applications
     * with a clearer signal earlier by preventing them from enabling ktls on a
     * connection that MIGHT require renegotiation. We can relax this restriction
     * later if necessary.
     */
    bool may_receive_hello_request = s2n_result_is_ok(s2n_client_hello_request_validate(conn));
    bool may_renegotiate = may_receive_hello_request && config->renegotiate_request_cb;
    RESULT_ENSURE(!may_renegotiate, S2N_ERR_KTLS_RENEG);

    /* Prevent kTLS from being enabled on connections that might be serialized.
     *
     * The socket takes over tracking sequence numbers when kTLS is enabled.
     * We would need to call getsockopt to retrieve the current sequence numbers for
     * serialization. This would complicate the serialization implementation so
     * for now, do not support kTLS with serialization.
     */
    RESULT_ENSURE(config->serialized_connection_version == S2N_SERIALIZED_CONN_NONE,
            S2N_ERR_KTLS_UNSUPPORTED_CONN);

    /* kTLS I/O functionality is managed by s2n-tls. kTLS cannot be enabled if the
     * application sets custom I/O (managed_send_io == false means application has
     * set custom I/O).
     */
    switch (ktls_mode) {
        case S2N_KTLS_MODE_SEND:
            RESULT_ENSURE(conn->managed_send_io, S2N_ERR_KTLS_MANAGED_IO);
            /* The output stuffer should be empty before enabling kTLS. */
            RESULT_ENSURE(s2n_stuffer_is_consumed(&conn->out), S2N_ERR_RECORD_STUFFER_NEEDS_DRAINING);
            break;
        case S2N_KTLS_MODE_RECV:
            RESULT_ENSURE(conn->managed_recv_io, S2N_ERR_KTLS_MANAGED_IO);
            /* The input stuffers should be empty before enabling kTLS. */
            RESULT_ENSURE(s2n_stuffer_is_consumed(&conn->header_in), S2N_ERR_RECORD_STUFFER_NEEDS_DRAINING);
            RESULT_ENSURE(s2n_stuffer_is_consumed(&conn->in), S2N_ERR_RECORD_STUFFER_NEEDS_DRAINING);
            RESULT_ENSURE(s2n_stuffer_is_consumed(&conn->buffer_in), S2N_ERR_KTLS_UNSUPPORTED_CONN);
            break;
        default:
            RESULT_BAIL(S2N_ERR_SAFETY);
            break;
    }

    return S2N_RESULT_OK;
}

/* Enabling kTLS preserves the original *io_context; making this functions
 * safe to call even after kTLS has been enabled on the connection.
 *
 * Retrieving fd assumes that the connection is using socket IO and has the
 * send_io_context set. While kTLS overrides IO and essentially disables
 * the socket conn->send function callback, it doesn't modify the
 * send_io_context. */
S2N_RESULT s2n_ktls_get_file_descriptor(struct s2n_connection *conn, s2n_ktls_mode ktls_mode, int *fd)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(fd);

    if (ktls_mode == S2N_KTLS_MODE_RECV) {
        RESULT_GUARD_POSIX(s2n_connection_get_read_fd(conn, fd));
    } else if (ktls_mode == S2N_KTLS_MODE_SEND) {
        RESULT_GUARD_POSIX(s2n_connection_get_write_fd(conn, fd));
    }
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_ktls_get_io_mode(s2n_ktls_mode ktls_mode, int *tls_tx_rx_mode)
{
    RESULT_ENSURE_REF(tls_tx_rx_mode);

    if (ktls_mode == S2N_KTLS_MODE_SEND) {
        *tls_tx_rx_mode = S2N_TLS_TX;
    } else {
        *tls_tx_rx_mode = S2N_TLS_RX;
    }
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_ktls_crypto_info_init(struct s2n_connection *conn, s2n_ktls_mode ktls_mode,
        struct s2n_ktls_crypto_info *crypto_info)
{
    RESULT_ENSURE_REF(conn);
    struct s2n_crypto_parameters *secure = conn->secure;
    RESULT_ENSURE_REF(secure);

    /* In order to avoid storing the encryption keys on the connection, we instead
     * regenerate them when required by ktls.
     *
     * s2n_key_material also includes an IV, but we should use the IV stored
     * on the connection instead. Some record algorithms (like CBC) mutate the
     * "implicit_iv" when writing records, so the IV may change after generation.
     */
    struct s2n_key_material key_material = { 0 };

    bool is_sending_key = (ktls_mode == S2N_KTLS_MODE_SEND);
    s2n_mode key_mode = (is_sending_key) ? conn->mode : S2N_PEER_MODE(conn->mode);

    switch (conn->actual_protocol_version) {
        case S2N_TLS12:
            RESULT_GUARD(s2n_prf_generate_key_material(conn, &key_material));
            break;
        case S2N_TLS13:
            RESULT_GUARD(s2n_tls13_key_schedule_generate_key_material(
                    conn, key_mode, &key_material));
            break;
        default:
            RESULT_BAIL(S2N_ERR_KTLS_UNSUPPORTED_CONN);
    }

    struct s2n_ktls_crypto_info_inputs inputs = { 0 };
    if (key_mode == S2N_CLIENT) {
        inputs.key = key_material.client_key;
        RESULT_GUARD_POSIX(s2n_blob_init(&inputs.iv,
                secure->client_implicit_iv, sizeof(secure->client_implicit_iv)));
    } else {
        inputs.key = key_material.server_key;
        RESULT_GUARD_POSIX(s2n_blob_init(&inputs.iv,
                secure->server_implicit_iv, sizeof(secure->server_implicit_iv)));
    }
    RESULT_GUARD(s2n_connection_get_sequence_number(conn, key_mode, &inputs.seq));

    const struct s2n_cipher *cipher = NULL;
    RESULT_GUARD(s2n_connection_get_secure_cipher(conn, &cipher));
    RESULT_ENSURE_REF(cipher);
    RESULT_ENSURE_REF(cipher->set_ktls_info);
    RESULT_GUARD(cipher->set_ktls_info(&inputs, crypto_info));
    return S2N_RESULT_OK;
}

/* This method intentionally returns void because it may NOT perform any fallible
 * operations. See s2n_connection_ktls_enable.
 */
void s2n_ktls_configure_connection(struct s2n_connection *conn, s2n_ktls_mode ktls_mode)
{
    if (conn == NULL) {
        return;
    }
    if (ktls_mode == S2N_KTLS_MODE_SEND) {
        conn->ktls_send_enabled = true;
        conn->send = s2n_ktls_send_cb;
    } else {
        conn->ktls_recv_enabled = true;
        conn->recv = s2n_ktls_disabled_read;
    }
}

static S2N_RESULT s2n_connection_ktls_enable(struct s2n_connection *conn, s2n_ktls_mode ktls_mode)
{
    RESULT_ENSURE_REF(conn);
    RESULT_GUARD(s2n_ktls_validate(conn, ktls_mode));

    int fd = 0;
    RESULT_GUARD(s2n_ktls_get_file_descriptor(conn, ktls_mode, &fd));

    /* This call doesn't actually enable ktls or modify the IO behavior of the socket.
     * Instead, this is just a prerequisite for calling setsockopt with SOL_TLS.
     *
     * We intentionally ignore the result of this call. It may fail because ktls
     * is not supported, but it might also fail because ktls has already been enabled
     * for the socket. If SOL_TLS isn't enabled on the socket, our next call to
     * setsockopt with SOL_TLS will also fail, and we DO check that result.
     */
    s2n_setsockopt(fd, S2N_SOL_TCP, S2N_TCP_ULP, S2N_TLS_ULP_NAME, S2N_TLS_ULP_NAME_SIZE);

    int tls_tx_rx_mode = 0;
    RESULT_GUARD(s2n_ktls_get_io_mode(ktls_mode, &tls_tx_rx_mode));

    struct s2n_ktls_crypto_info crypto_info = { 0 };
    RESULT_GUARD(s2n_ktls_crypto_info_init(conn, ktls_mode, &crypto_info));

    /* If this call succeeds, then ktls is enabled for that io mode and will be offloaded */
    int ret = s2n_setsockopt(fd, S2N_SOL_TLS, tls_tx_rx_mode, crypto_info.value.data, crypto_info.value.size);
    RESULT_ENSURE(ret == 0, S2N_ERR_KTLS_ENABLE);

    /* At this point, ktls is enabled on the socket for the requested IO mode.
     * No further fallible operations may be performed, or else the caller may
     * incorrectly assume that enabling ktls failed and they should therefore
     * fall back to using application layer TLS.
     *
     * That means no calls to RESULT_ENSURE, RESULT_GUARD, etc. after this point.
     */

    s2n_ktls_configure_connection(conn, ktls_mode);
    return S2N_RESULT_OK;
}

int s2n_connection_ktls_enable_send(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);

    /* If already enabled then return success */
    if (conn->ktls_send_enabled) {
        return S2N_SUCCESS;
    }

    POSIX_GUARD_RESULT(s2n_connection_ktls_enable(conn, S2N_KTLS_MODE_SEND));
    return S2N_SUCCESS;
}

int s2n_connection_ktls_enable_recv(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);

    /* If already enabled then return success */
    if (conn->ktls_recv_enabled) {
        return S2N_SUCCESS;
    }

    POSIX_GUARD_RESULT(s2n_connection_ktls_enable(conn, S2N_KTLS_MODE_RECV));
    return S2N_SUCCESS;
}

int s2n_config_ktls_enable_unsafe_tls13(struct s2n_config *config)
{
    POSIX_ENSURE_REF(config);
    config->ktls_tls13_enabled = true;
    return S2N_SUCCESS;
}
