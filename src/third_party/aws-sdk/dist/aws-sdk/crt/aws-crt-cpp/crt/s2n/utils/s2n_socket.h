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

#include "tls/s2n_connection.h"

/* The default read I/O context for communication over a socket */
struct s2n_socket_read_io_context {
    /* The peer's fd */
    int fd;

    /* Has TCP_QUICKACK been set since the last read */
    unsigned int tcp_quickack_set : 1;
    /* Original SO_RCVLOWAT socket option settings before s2n takes over the fd */
    unsigned int original_rcvlowat_is_set : 1;
    int original_rcvlowat_val;
};

/* The default write I/O context for communication over a socket */
struct s2n_socket_write_io_context {
    /* The peer's fd */
    int fd;

    /* Original TCP_CORK socket option settings before s2n takes over the fd */
    unsigned int original_cork_is_set : 1;
    int original_cork_val;
};

int s2n_socket_quickack(struct s2n_connection *conn);
int s2n_socket_read_snapshot(struct s2n_connection *conn);
int s2n_socket_write_snapshot(struct s2n_connection *conn);
int s2n_socket_read_restore(struct s2n_connection *conn);
int s2n_socket_write_restore(struct s2n_connection *conn);
int s2n_socket_was_corked(struct s2n_connection *conn);
int s2n_socket_write_cork(struct s2n_connection *conn);
int s2n_socket_write_uncork(struct s2n_connection *conn);
int s2n_socket_set_read_size(struct s2n_connection *conn, int size);
int s2n_socket_read(void *io_context, uint8_t *buf, uint32_t len);
int s2n_socket_write(void *io_context, const uint8_t *buf, uint32_t len);
int s2n_socket_is_ipv6(int fd, uint8_t *ipv6);
