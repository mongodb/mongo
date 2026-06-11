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

#include <s2n.h>

/**
 * @file renegotiate.h
 *
 * "Renegotiation" is a TLS feature offered in TLS1.2 and earlier.
 * During renegotiation, a new handshake is performed on an already established
 * connection. The new handshake is encrypted using the keys from the original handshake.
 * The new handshake may not match the first handshake; for example, the server may choose
 * a different cipher suite or require client authentication for the new handshake.
 *
 * s2n-tls clients support secure (compliant with RFC5746) renegotiation for compatibility reasons,
 * but s2n-tls does NOT recommend its use. While s2n-tls addresses all currently known security concerns,
 * renegotiation has appeared in many CVEs and was completely removed from TLS1.3.
 *
 * A basic renegotiation integration with s2n-tls would look like:
 *  1. The application calls s2n_recv as part of normal IO.
 *  2. s2n_recv receives a request for renegotiation (a HelloRequest message)
 *     instead of application data.
 *  3. s2n_recv calls the configured s2n_renegotiate_request_cb.
 *  4. The application's implementation of the s2n_renegotiate_request_cb should:
 *     1. Set the `response` parameter to S2N_RENEGOTIATE_ACCEPT
 *     2. Set some application state to indicate that renegotiation is required.
 *        s2n_connection_set_ctx can be used to associate application state with
 *        a specific connection.
 *     3. Return success.
 *  5. s2n_recv returns as part of normal IO.
 *  6. The application should check the application state set in 4.2 to determine
 *     whether or not renegotiation is required.
 *  7. The application should complete any in-progress IO. Failing to do this will
 *     cause s2n_renegotiate_wipe to fail.
 *     1. For sending, the application must retry any blocked calls to s2n_send
 *        until they return success.
 *     2. For receiving, the application must call s2n_recv to handle any buffered
 *        decrypted application data. s2n_peek indicates how much data is buffered.
 *  8. The application should call s2n_renegotiate_wipe.
 *  9. The application should reconfigure the connection, if necessary.
 * 10. The application should call s2n_renegotiate until it indicates success,
 *     while handling any application data encountered.
 */

/**
 * Used to indicate that an attempt to renegotiate encountered
 * application data which the application should process before
 * continuing the handshake.
 */
extern const s2n_blocked_status S2N_BLOCKED_ON_APPLICATION_DATA;

/**
 * Indicates how a renegotiation request should be handled.
 */
typedef enum {
    /* The client will take no action */
    S2N_RENEGOTIATE_IGNORE = 0,
    /* The client will send a warning alert to the server */
    S2N_RENEGOTIATE_REJECT,
    /* The client will begin renegotiation in the future */
    S2N_RENEGOTIATE_ACCEPT,
} s2n_renegotiate_response;

/**
 * Callback function to handle requests for renegotiation.
 *
 * s2n-tls calls this method when a client receives a request from the server
 * to renegotiate the connection. If the server makes multiple requests,
 * s2n-tls will call this method multiple times.
 *
 * Applications should use the `response` value to indicate how the request
 * should be handled. If `response` is set to `S2N_RENEGOTIATE_IGNORE`
 * or `S2N_RENEGOTIATE_REJECT`, no further application involvement is required.
 *
 * If `response` is set to `S2N_RENEGOTIATE_ACCEPT`, then the application should
 * handle renegotiation. The application should stop calling s2n_send and s2n_recv,
 * wipe the connection with s2n_renegotiate_wipe, and then call s2n_renegotiate
 * until the handshake is complete.
 *
 * @param conn A pointer to the connection object.
 * @param context Context for the callback function.
 * @param response How the request should be handled.
 * @returns S2N_SUCCESS on success, S2N_FAILURE on error.
 */
typedef int (*s2n_renegotiate_request_cb)(struct s2n_connection *conn, void *context, s2n_renegotiate_response *response);

/**
 * Sets a method to be called when the client receives a request to renegotiate.
 *
 * @param config A pointer to the config object.
 * @param callback The function to be called when a renegotiation request is received.
 * @param context Context to be passed to the callback function.
 * @returns S2N_SUCCESS on success, S2N_FAILURE on error.
 */
S2N_API int s2n_config_set_renegotiate_request_cb(struct s2n_config *config, s2n_renegotiate_request_cb callback, void *context);

/**
 * Reset the connection so that it can be renegotiated.
 *
 * Similar to `s2n_connection_wipe`, this method resets a connection so that it can be used again.
 * However, unlike `s2n_connection_wipe`, it retains enough state from the previous connection
 * that the connection can continue to send and receive data encrypted with the old keys.
 *
 * The application MUST handle any incomplete IO before calling this method. The last call to `s2n_send` must
 * have succeeded, and `s2n_peek` must return zero. If there is any data in the send or receive buffers,
 * this method will fail. That means that this method cannot be called from inside s2n_renegotiate_request_cb.
 *
 * The application MUST repeat any connection-specific setup after calling this method. This method
 * cannot distinguish between internal connection state and configuration state set by the application,
 * so it wipes all state not directly related to handling encrypted records. For example,
 * if the application originally called `s2n_connection_set_blinding` on the connection,
 * then the application will need to call `s2n_connection_set_blinding` again after `s2n_renegotiate_wipe`.
 *
 * The connection-specific setup methods the application does not need to call again are:
 * - Methods to set the file descriptors
 *   (`s2n_connection_set_fd`, `s2n_connection_set_read_fd`, `s2n_connection_set_write_fd`)
 * - Methods to set the send callback
 *   (`s2n_connection_set_send_cb`, `s2n_connection_set_send_ctx`)
 * - Methods to set the recv callback
 *   (`s2n_connection_set_recv_cb`, `s2n_connection_set_recv_ctx`)
 *
 * @note This method MUST be called before s2n_renegotiate.
 * @note Calling this method on a server connection will fail. s2n-tls servers do not support renegotiation.
 * @note This method will fail if the connection has indicated that it will be serialized with
 * `s2n_config_set_serialization_version()`.
 *
 * @param conn A pointer to the connection object.
 * @returns S2N_SUCCESS on success, S2N_FAILURE on error.
 */
S2N_API int s2n_renegotiate_wipe(struct s2n_connection *conn);

/**
 * Perform a new handshake on an already established connection.
 *
 * This method should be called like `s2n_negotiate`, with the same handling of return values,
 * error types, and blocked statuses.
 *
 * However, unlike the initial handshake performed by `s2n_negotiate`, the renegotiation
 * handshake can encounter valid application data. In that case, this method will fail
 * with an error of type S2N_ERR_T_BLOCKED, set the `blocked` field to `S2N_BLOCKED_ON_APPLICATION_DATA`,
 * copy the data to `app_data_buf`, and set `app_data_size` to the size of the data.
 * The application should handle the data in `app_data_buf` before calling s2n_renegotiate again.
 *
 * This method cannot be called from inside s2n_renegotiate_request_cb. The receive
 * call that triggered s2n_renegotiate_request_cb must complete before either
 * s2n_renegotiate_wipe or s2n_renegotiate can be called.
 *
 * @note s2n_renegotiate_wipe MUST be called before this method.
 * @note Calling this method on a server connection will fail. s2n-tls servers do not support renegotiation.
 *
 * @param conn A pointer to the connection object.
 * @param app_data_buf A pointer to a buffer that s2n will copy application data read into.
 * @param app_data_buf_size The size of `app_data_buf`.
 * @param app_data_size The number of application data bytes read.
 * @param blocked A pointer which will be set to the blocked status.
 * @returns S2N_SUCCESS if the handshake completed. S2N_FAILURE if the handshake encountered an error or is blocked.
 */
S2N_API int s2n_renegotiate(struct s2n_connection *conn, uint8_t *app_data_buf, ssize_t app_data_buf_size,
        ssize_t *app_data_size, s2n_blocked_status *blocked);
