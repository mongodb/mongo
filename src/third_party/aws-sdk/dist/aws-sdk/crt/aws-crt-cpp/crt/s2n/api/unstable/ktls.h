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
 * @file ktls.h
 *
 * The following APIs enable applications to use kernel TLS (kTLS), meaning that
 * encrypting and decrypting TLS records is handled by the kernel rather than by
 * the s2n-tls library.
 *
 * The kTLS APIs are currently considered unstable. kTLS is a relatively new
 * feature with limited and volatile support from different kernels and hardware.
 *
 * Currently, s2n-tls supports ktls for limited scenarios:
 * - You must be using Linux. We have not tested with other kernels.
 * - Your kernel must support kTLS. For Linux, versions >4.13 should support kTLS.
 * - The TLS kernel module must be enabled. While some environments enable the
 *   module by default, most will require you to run `sudo modprobe tls`.
 * - You must negotiate AES128-GCM or AES256-GCM. Other ciphers are supported by
 *   the kernel, but not implemented in s2n-tls yet.
 * - You must not use the s2n_renegotiate_request_cb from unstable/negotiate.h.
 *   The TLS kernel module currently doesn't support renegotiation.
 * - By default, you must negotiate TLS1.2. See s2n_config_ktls_enable_tls13
 *   for the requirements to also support TLS1.3.
 * - You must not use s2n_connection_set_recv_buffering
 */

/**
 * Enables sending using kTLS on a given connection.
 *
 * See above for the limitations on when kTLS can be enabled. Additionally,
 * s2n_connection_ktls_enable_send must be called after the handshake completes
 * but before the handshake is freed with s2n_connection_free_handshake.
 * It may be called after some application data is sent and received without kTLS,
 * but there must be no pending application data that requires flushing. If these
 * requirements are not met, enabling kTLS will fail with an error.
 *
 * After kTLS is enabled for sending, s2n_send, s2n_sendv, and s2n_sendv_with_offset
 * will use kTLS. kTLS should result in memory and CPU savings. s2n_sendfile will
 * also become available.
 *
 * For applications using kTLS to avoid copying or allocating memory, s2n_sendv
 * should be preferred over s2n_sendv_with_offset. For s2n_sendv_with_offset,
 * s2n-tls may need to copy the provided iovec array to apply the offset, and may
 * need to allocate memory to copy large (>16) iovec arrays.
 *
 * If kTLS is enabled for sending, s2n_connection_get_wire_bytes_out will always
 * return 0 instead of an accurate count.
 *
 * @warning Due to the uncertainty around kTLS support, the signature of this
 * method is likely to change before kTLS is marked as stable.
 *
 * @param conn A pointer to the connection.
 * @returns S2N_SUCCESS if kTLS is successfully enabled. If kTlS is not successfully
 * enabled, returns S2N_FAILURE but the connection may proceed without kTLS.
 */
S2N_API int s2n_connection_ktls_enable_send(struct s2n_connection *conn);

/**
 * Enables receiving using kTLS on a given connection.
 *
 * See above for the limitations on when kTLS can be enabled. Additionally,
 * s2n_connection_ktls_enable_recv must be called after the handshake completes
 * but before the handshake is freed with s2n_connection_free_handshake.
 * It may be called after some application data is sent and received without kTLS,
 * but there must be no buffered application data that requires draining. If these
 * requirements are not met, enabling kTLS will fail with an error.
 *
 * After kTLS is enabled for receiving, s2n_recv will use kTLS. This may result
 * in memory and CPU savings, but currently will still buffer and copy application data.
 * We will further optimize s2n_recv for kTLS in the future.
 *
 * If kTLS is enabled for receiving, s2n_connection_get_wire_bytes_in will always
 * return 0 instead of an accurate count.
 *
 * @warning Due to the uncertainty around kTLS support, the signature of this
 * method is likely to change before kTLS is marked as stable.
 *
 * @param conn A pointer to the connection.
 * @returns S2N_SUCCESS if kTLS is successfully enabled. If kTlS is not successfully
 * enabled, returns S2N_FAILURE but the connection may proceed without kTLS.
 */
S2N_API int s2n_connection_ktls_enable_recv(struct s2n_connection *conn);

/**
 * Allows kTLS to be enabled if a connection negotiates TLS1.3.
 *
 * Enabling TLS1.3 with this method is considered "unsafe" because the kernel
 * currently doesn't support updating encryption keys, which is required in TLS1.3.
 * s2n_connection_get_key_update_counts can be used to gather metrics on whether
 * key updates are occurring on your connections before enabling TLS1.3.
 *
 * In order to safely enable TLS1.3, an application must ensure that its peer will
 * not send any KeyUpdate messages. If s2n-tls receives a KeyUpdate message while
 * kTLS is enabled, it will report an S2N_ERR_KTLS_KEYUPDATE S2N_ERR_T_PROTO error.
 *
 * Additionally, an application must not use kTLS to attempt to send more than 35GB
 * of data and must not call s2n_send more than 23 million times. If either of these
 * limits is exceeded, it will report an S2N_ERR_KTLS_KEY_LIMIT S2N_ERR_T_PROTO error.
 *
 * This method must be called before enabling kTLS on a connection using
 * s2n_connection_ktls_enable_send or s2n_connection_ktls_enable_recv.
 *
 * @param config A pointer to the config.
 * @returns S2N_SUCCESS if successfully enabled, S2N_FAILURE otherwise.
 */
S2N_API int s2n_config_ktls_enable_unsafe_tls13(struct s2n_config *config);

/**
 * Reports the number of times sending and receiving keys have been updated.
 *
 * This only applies to TLS1.3. Earlier versions do not support key updates.
 *
 * @warning s2n-tls only tracks up to UINT8_MAX (255) key updates. If this method
 * reports 255 updates, then more than 255 updates may have occurred.
 *
 * @param conn A pointer to the connection.
 * @param send_key_updates Number of times the sending key was updated.
 * @param recv_key_updates Number of times the receiving key was updated.
 * @returns S2N_SUCCESS if successful, S2N_FAILURE otherwise.
 */
S2N_API int s2n_connection_get_key_update_counts(struct s2n_connection *conn,
        uint8_t *send_key_updates, uint8_t *recv_key_updates);

/**
 * Sends the contents of a file as application data.
 *
 * s2n_sendfile should be more efficient than s2n_send because the copy between
 * the file and the write socket happens inside the kernel.
 *
 * This method is only supported if kTLS is enabled for sending.
 *
 * @note For a TLS1.3 connection, the `count` argument will be used to enforce
 * safe sending limits regardless of the actual size of the file. Applications
 * should not set `count` excessively high.
 *
 * @param conn A pointer to the connection.
 * @param fd The file descriptor to read from. It must be opened for reading and
 * support mmap-like operations (i.e., it cannot be a socket).
 * @param offset The offset in the file to begin reading at.
 * @param count The maximum number of bytes to read from the file.
 * @param bytes_written Will be set to the number of bytes written if successful.
 * @param blocked Will be set to the blocked status if an `S2N_ERR_T_BLOCKED` error is returned.
 * @returns S2N_SUCCESS if any bytes are successfully written, S2N_FAILURE otherwise.
 */
S2N_API int s2n_sendfile(struct s2n_connection *conn, int fd, off_t offset, size_t count,
        size_t *bytes_written, s2n_blocked_status *blocked);
