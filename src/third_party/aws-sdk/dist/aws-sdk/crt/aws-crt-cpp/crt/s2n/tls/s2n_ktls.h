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

#include <sys/socket.h>

#include "api/unstable/ktls.h"
#include "tls/s2n_connection.h"
/* Define headers needed to enable and use kTLS.
 *
 * The inline header definitions are required to compile kTLS specific code.
 * kTLS has been tested on linux. For all other platforms, kTLS is marked as
 * unsupported, and will return an unsupported error.
 */
#include "tls/s2n_ktls_parameters.h"

/* A set of kTLS configurations representing the combination of sending
 * and receiving.
 */
typedef enum {
    /* Enable kTLS for the send socket. */
    S2N_KTLS_MODE_SEND,
    /* Enable kTLS for the receive socket. */
    S2N_KTLS_MODE_RECV,
} s2n_ktls_mode;

bool s2n_ktls_is_supported_on_platform();
S2N_RESULT s2n_ktls_get_file_descriptor(struct s2n_connection *conn, s2n_ktls_mode ktls_mode, int *fd);

int s2n_ktls_send_cb(void *io_context, const uint8_t *buf, uint32_t len);
S2N_RESULT s2n_ktls_sendmsg(void *io_context, uint8_t record_type, const struct iovec *msg_iov,
        size_t msg_iovlen, s2n_blocked_status *blocked, size_t *bytes_written);
S2N_RESULT s2n_ktls_recvmsg(void *io_context, uint8_t *record_type, uint8_t *buf,
        size_t buf_len, s2n_blocked_status *blocked, size_t *bytes_read);

ssize_t s2n_ktls_sendv_with_offset(struct s2n_connection *conn, const struct iovec *bufs,
        ssize_t count, ssize_t offs, s2n_blocked_status *blocked);
int s2n_ktls_record_writev(struct s2n_connection *conn, uint8_t content_type,
        const struct iovec *in, int in_count, size_t offs, size_t to_write);
int s2n_ktls_read_full_record(struct s2n_connection *conn, uint8_t *record_type);

/* Testing */
typedef int (*s2n_setsockopt_fn)(int socket, int level, int option_name, const void *option_value,
        socklen_t option_len);
S2N_RESULT s2n_ktls_set_setsockopt_cb(s2n_setsockopt_fn cb);
typedef ssize_t (*s2n_ktls_sendmsg_fn)(void *io_context, const struct msghdr *msg);
typedef ssize_t (*s2n_ktls_recvmsg_fn)(void *io_context, struct msghdr *msg);
S2N_RESULT s2n_ktls_set_sendmsg_cb(struct s2n_connection *conn, s2n_ktls_sendmsg_fn send_cb,
        void *send_ctx);
S2N_RESULT s2n_ktls_set_recvmsg_cb(struct s2n_connection *conn, s2n_ktls_recvmsg_fn recv_cb,
        void *recv_ctx);
void s2n_ktls_configure_connection(struct s2n_connection *conn, s2n_ktls_mode ktls_mode);

/* These functions will be part of the public API. */
int s2n_connection_ktls_enable_send(struct s2n_connection *conn);
int s2n_connection_ktls_enable_recv(struct s2n_connection *conn);
int s2n_sendfile(struct s2n_connection *conn, int in_fd, off_t offset, size_t count,
        size_t *bytes_written, s2n_blocked_status *blocked);
