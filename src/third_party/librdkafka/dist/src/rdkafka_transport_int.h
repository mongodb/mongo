/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2015, Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef _RDKAFKA_TRANSPORT_INT_H_
#define _RDKAFKA_TRANSPORT_INT_H_

/* This header file is to be used by .c files needing access to the
 * rd_kafka_transport_t struct internals. */

#include "rdkafka_sasl.h"

#if WITH_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pkcs12.h>
#endif

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/tcp.h>
#endif

struct rd_kafka_transport_s {
        rd_socket_t rktrans_s;
        rd_kafka_broker_t *rktrans_rkb; /* Not reference counted */

#if WITH_SSL
        SSL *rktrans_ssl;
#endif

#ifdef _WIN32
        WSAEVENT *rktrans_wsaevent;
        rd_bool_t rktrans_blocked; /* Latest send() returned ..WOULDBLOCK.
                                    * We need to poll for FD_WRITE which
                                    * is edge-triggered rather than
                                    * level-triggered.
                                    * This behaviour differs from BSD
                                    * sockets. */
#endif

        struct {
                void *state; /* SASL implementation
                              * state handle */

                int complete; /* Auth was completed early
                               * from the client's perspective
                               * (but we might still have to
                               *  wait for server reply). */

                /* SASL framing buffers */
                struct msghdr msg;
                struct iovec iov[2];

                char *recv_buf;
                int recv_of;  /* Received byte count */
                int recv_len; /* Expected receive length for
                               * current frame. */
        } rktrans_sasl;

        rd_kafka_buf_t *rktrans_recv_buf; /* Used with framed_recvmsg */

        /* Two pollable fds:
         * - TCP socket
         * - wake-up fd  (not used on Win32)
         */
        rd_pollfd_t rktrans_pfd[2];
        int rktrans_pfd_cnt;

        size_t rktrans_rcvbuf_size; /**< Socket receive buffer size */
        size_t rktrans_sndbuf_size; /**< Socket send buffer size */
};


extern RD_TLS rd_kafka_transport_t *rd_kafka_curr_transport;

#endif /* _RDKAFKA_TRANSPORT_INT_H_ */
