/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2015-2022, Magnus Edenhill
 *               2023, Confluent Inc.
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
#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

#define __need_IOV_MAX

#define _DARWIN_C_SOURCE /* MSG_DONTWAIT */

#include "rdkafka_int.h"
#include "rdaddr.h"
#include "rdkafka_transport.h"
#include "rdkafka_transport_int.h"
#include "rdkafka_broker.h"
#include "rdkafka_interceptor.h"

#include <errno.h>

/* AIX doesn't have MSG_DONTWAIT */
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT MSG_NONBLOCK
#endif

#if WITH_SSL
#include "rdkafka_ssl.h"
#endif

/**< Current thread's rd_kafka_transport_t instance.
 *   This pointer is set up when calling any OpenSSL APIs that might
 *   trigger SSL callbacks, and is used to retrieve the SSL object's
 *   corresponding rd_kafka_transport_t instance.
 *   There is an set/get_ex_data() API in OpenSSL, but it requires storing
 *   a unique index somewhere, which we can't do without having a singleton
 *   object, so instead we cut out the middle man and store the
 *   rd_kafka_transport_t pointer directly in the thread-local memory. */
RD_TLS rd_kafka_transport_t *rd_kafka_curr_transport;


static int rd_kafka_transport_poll(rd_kafka_transport_t *rktrans, int tmout);


/**
 * Low-level socket close
 */
static void rd_kafka_transport_close0(rd_kafka_t *rk, rd_socket_t s) {
        if (rk->rk_conf.closesocket_cb)
                rk->rk_conf.closesocket_cb((int)s, rk->rk_conf.opaque);
        else
                rd_socket_close(s);
}

/**
 * Close and destroy a transport handle
 */
void rd_kafka_transport_close(rd_kafka_transport_t *rktrans) {
#if WITH_SSL
        rd_kafka_curr_transport = rktrans;
        if (rktrans->rktrans_ssl)
                rd_kafka_transport_ssl_close(rktrans);
#endif

        rd_kafka_sasl_close(rktrans);

        if (rktrans->rktrans_recv_buf)
                rd_kafka_buf_destroy(rktrans->rktrans_recv_buf);

#ifdef _WIN32
        WSACloseEvent(rktrans->rktrans_wsaevent);
#endif

        if (rktrans->rktrans_s != -1)
                rd_kafka_transport_close0(rktrans->rktrans_rkb->rkb_rk,
                                          rktrans->rktrans_s);

        rd_free(rktrans);
}

/**
 * @brief shutdown(2) a transport's underlying socket.
 *
 * This will prohibit further sends and receives.
 * rd_kafka_transport_close() must still be called to close the socket.
 */
void rd_kafka_transport_shutdown(rd_kafka_transport_t *rktrans) {
        shutdown(rktrans->rktrans_s,
#ifdef _WIN32
                 SD_BOTH
#else
                 SHUT_RDWR
#endif
        );
}


#ifndef _WIN32
/**
 * @brief sendmsg() abstraction, converting a list of segments to iovecs.
 * @remark should only be called if the number of segments is > 1.
 */
static ssize_t rd_kafka_transport_socket_sendmsg(rd_kafka_transport_t *rktrans,
                                                 rd_slice_t *slice,
                                                 char *errstr,
                                                 size_t errstr_size) {
        struct iovec iov[IOV_MAX];
        struct msghdr msg = {.msg_iov = iov};
        size_t iovlen;
        ssize_t r;
        size_t r2;

        rd_slice_get_iov(slice, msg.msg_iov, &iovlen, IOV_MAX,
                         /* FIXME: Measure the effects of this */
                         rktrans->rktrans_sndbuf_size);
        msg.msg_iovlen = (int)iovlen;

#ifdef __sun
        /* See recvmsg() comment. Setting it here to be safe. */
        rd_socket_errno = EAGAIN;
#endif

        r = sendmsg(rktrans->rktrans_s, &msg,
                    MSG_DONTWAIT
#ifdef MSG_NOSIGNAL
                        | MSG_NOSIGNAL
#endif
        );

        if (r == -1) {
                if (rd_socket_errno == EAGAIN)
                        return 0;
                rd_snprintf(errstr, errstr_size, "%s", rd_strerror(errno));
                return -1;
        }

        /* Update buffer read position */
        r2 = rd_slice_read(slice, NULL, (size_t)r);
        rd_assert((size_t)r == r2 &&
                  *"BUG: wrote more bytes than available in slice");

        return r;
}
#endif


/**
 * @brief Plain send() abstraction
 */
static ssize_t rd_kafka_transport_socket_send0(rd_kafka_transport_t *rktrans,
                                               rd_slice_t *slice,
                                               char *errstr,
                                               size_t errstr_size) {
        ssize_t sum = 0;
        const void *p;
        size_t rlen;

        while ((rlen = rd_slice_peeker(slice, &p))) {
                ssize_t r;
                size_t r2;

                r = send(rktrans->rktrans_s, p,
#ifdef _WIN32
                         (int)rlen, (int)0
#else
                         rlen, 0
#endif
                );

#ifdef _WIN32
                if (unlikely(r == RD_SOCKET_ERROR)) {
                        if (sum > 0 || rd_socket_errno == WSAEWOULDBLOCK) {
                                rktrans->rktrans_blocked = rd_true;
                                return sum;
                        } else {
                                rd_snprintf(
                                    errstr, errstr_size, "%s",
                                    rd_socket_strerror(rd_socket_errno));
                                return -1;
                        }
                }

                rktrans->rktrans_blocked = rd_false;
#else
                if (unlikely(r <= 0)) {
                        if (r == 0 || rd_socket_errno == EAGAIN)
                                return 0;
                        rd_snprintf(errstr, errstr_size, "%s",
                                    rd_socket_strerror(rd_socket_errno));
                        return -1;
                }
#endif

                /* Update buffer read position */
                r2 = rd_slice_read(slice, NULL, (size_t)r);
                rd_assert((size_t)r == r2 &&
                          *"BUG: wrote more bytes than available in slice");


                sum += r;

                /* FIXME: remove this and try again immediately and let
                 *        the next write() call fail instead? */
                if ((size_t)r < rlen)
                        break;
        }

        return sum;
}


static ssize_t rd_kafka_transport_socket_send(rd_kafka_transport_t *rktrans,
                                              rd_slice_t *slice,
                                              char *errstr,
                                              size_t errstr_size) {
#ifndef _WIN32
        /* FIXME: Use sendmsg() with iovecs if there's more than one segment
         * remaining, otherwise (or if platform does not have sendmsg)
         * use plain send(). */
        return rd_kafka_transport_socket_sendmsg(rktrans, slice, errstr,
                                                 errstr_size);
#endif
        return rd_kafka_transport_socket_send0(rktrans, slice, errstr,
                                               errstr_size);
}



#ifndef _WIN32
/**
 * @brief recvmsg() abstraction, converting a list of segments to iovecs.
 * @remark should only be called if the number of segments is > 1.
 */
static ssize_t rd_kafka_transport_socket_recvmsg(rd_kafka_transport_t *rktrans,
                                                 rd_buf_t *rbuf,
                                                 char *errstr,
                                                 size_t errstr_size) {
        ssize_t r;
        struct iovec iov[IOV_MAX];
        struct msghdr msg = {.msg_iov = iov};
        size_t iovlen;

        rd_buf_get_write_iov(rbuf, msg.msg_iov, &iovlen, IOV_MAX,
                             /* FIXME: Measure the effects of this */
                             rktrans->rktrans_rcvbuf_size);
        msg.msg_iovlen = (int)iovlen;

#ifdef __sun
        /* SunOS doesn't seem to set errno when recvmsg() fails
         * due to no data and MSG_DONTWAIT is set. */
        rd_socket_errno = EAGAIN;
#endif
        r = recvmsg(rktrans->rktrans_s, &msg, MSG_DONTWAIT);
        if (unlikely(r <= 0)) {
                if (r == -1 && rd_socket_errno == EAGAIN)
                        return 0;
                else if (r == 0 || (r == -1 && rd_socket_errno == ECONNRESET)) {
                        /* Receive 0 after POLLIN event means
                         * connection closed. */
                        rd_snprintf(errstr, errstr_size, "Disconnected");
                        return -1;
                } else if (r == -1) {
                        rd_snprintf(errstr, errstr_size, "%s",
                                    rd_strerror(errno));
                        return -1;
                }
        }

        /* Update buffer write position */
        rd_buf_write(rbuf, NULL, (size_t)r);

        return r;
}
#endif


/**
 * @brief Plain recv()
 */
static ssize_t rd_kafka_transport_socket_recv0(rd_kafka_transport_t *rktrans,
                                               rd_buf_t *rbuf,
                                               char *errstr,
                                               size_t errstr_size) {
        ssize_t sum = 0;
        void *p;
        size_t len;

        while ((len = rd_buf_get_writable(rbuf, &p))) {
                ssize_t r;

                r = recv(rktrans->rktrans_s, p,
#ifdef _WIN32
                         (int)
#endif
                             len,
                         0);

                if (unlikely(r == RD_SOCKET_ERROR)) {
                        if (rd_socket_errno == EAGAIN
#ifdef _WIN32
                            || rd_socket_errno == WSAEWOULDBLOCK
#endif
                        )
                                return sum;
                        else {
                                rd_snprintf(
                                    errstr, errstr_size, "%s",
                                    rd_socket_strerror(rd_socket_errno));
                                return -1;
                        }
                } else if (unlikely(r == 0)) {
                        /* Receive 0 after POLLIN event means
                         * connection closed. */
                        rd_snprintf(errstr, errstr_size, "Disconnected");
                        return -1;
                }

                /* Update buffer write position */
                rd_buf_write(rbuf, NULL, (size_t)r);

                sum += r;

                /* FIXME: remove this and try again immediately and let
                 *        the next recv() call fail instead? */
                if ((size_t)r < len)
                        break;
        }
        return sum;
}


static ssize_t rd_kafka_transport_socket_recv(rd_kafka_transport_t *rktrans,
                                              rd_buf_t *buf,
                                              char *errstr,
                                              size_t errstr_size) {
#ifndef _WIN32
        return rd_kafka_transport_socket_recvmsg(rktrans, buf, errstr,
                                                 errstr_size);
#endif
        return rd_kafka_transport_socket_recv0(rktrans, buf, errstr,
                                               errstr_size);
}



/**
 * CONNECT state is failed (errstr!=NULL) or done (TCP is up, SSL is working..).
 * From this state we either hand control back to the broker code,
 * or if authentication is configured we ente the AUTH state.
 */
void rd_kafka_transport_connect_done(rd_kafka_transport_t *rktrans,
                                     char *errstr) {
        rd_kafka_broker_t *rkb = rktrans->rktrans_rkb;

        rd_kafka_curr_transport = rktrans;

        rd_kafka_broker_connect_done(rkb, errstr);
}



ssize_t rd_kafka_transport_send(rd_kafka_transport_t *rktrans,
                                rd_slice_t *slice,
                                char *errstr,
                                size_t errstr_size) {
        ssize_t r;
#if WITH_SSL
        if (rktrans->rktrans_ssl) {
                rd_kafka_curr_transport = rktrans;
                r = rd_kafka_transport_ssl_send(rktrans, slice, errstr,
                                                errstr_size);
        } else
#endif
                r = rd_kafka_transport_socket_send(rktrans, slice, errstr,
                                                   errstr_size);

        return r;
}


ssize_t rd_kafka_transport_recv(rd_kafka_transport_t *rktrans,
                                rd_buf_t *rbuf,
                                char *errstr,
                                size_t errstr_size) {
        ssize_t r;

#if WITH_SSL
        if (rktrans->rktrans_ssl) {
                rd_kafka_curr_transport = rktrans;
                r = rd_kafka_transport_ssl_recv(rktrans, rbuf, errstr,
                                                errstr_size);
        } else
#endif
                r = rd_kafka_transport_socket_recv(rktrans, rbuf, errstr,
                                                   errstr_size);

        return r;
}



/**
 * @brief Notify transport layer of full request sent.
 */
void rd_kafka_transport_request_sent(rd_kafka_broker_t *rkb,
                                     rd_kafka_buf_t *rkbuf) {
        rd_kafka_transport_t *rktrans = rkb->rkb_transport;

        /* Call on_request_sent interceptors */
        rd_kafka_interceptors_on_request_sent(
            rkb->rkb_rk, (int)rktrans->rktrans_s, rkb->rkb_name,
            rkb->rkb_nodeid, rkbuf->rkbuf_reqhdr.ApiKey,
            rkbuf->rkbuf_reqhdr.ApiVersion, rkbuf->rkbuf_corrid,
            rd_slice_size(&rkbuf->rkbuf_reader));
}



/**
 * Length framed receive handling.
 * Currently only supports a the following framing:
 *     [int32_t:big_endian_length_of_payload][payload]
 *
 * To be used on POLLIN event, will return:
 *   -1: on fatal error (errstr will be updated, *rkbufp remains unset)
 *    0: still waiting for data (*rkbufp remains unset)
 *    1: data complete, (buffer returned in *rkbufp)
 */
int rd_kafka_transport_framed_recv(rd_kafka_transport_t *rktrans,
                                   rd_kafka_buf_t **rkbufp,
                                   char *errstr,
                                   size_t errstr_size) {
        rd_kafka_buf_t *rkbuf = rktrans->rktrans_recv_buf;
        ssize_t r;
        const int log_decode_errors = LOG_ERR;

        /* States:
         *   !rktrans_recv_buf: initial state; set up buf to receive header.
         *    rkbuf_totlen == 0:   awaiting header
         *    rkbuf_totlen > 0:    awaiting payload
         */

        if (!rkbuf) {
                rkbuf = rd_kafka_buf_new(1, 4 /*length field's length*/);
                /* Set up buffer reader for the length field */
                rd_buf_write_ensure(&rkbuf->rkbuf_buf, 4, 4);
                rktrans->rktrans_recv_buf = rkbuf;
        }


        r = rd_kafka_transport_recv(rktrans, &rkbuf->rkbuf_buf, errstr,
                                    errstr_size);
        if (r == 0)
                return 0;
        else if (r == -1)
                return -1;

        if (rkbuf->rkbuf_totlen == 0) {
                /* Frame length not known yet. */
                int32_t frame_len;

                if (rd_buf_write_pos(&rkbuf->rkbuf_buf) < sizeof(frame_len)) {
                        /* Wait for entire frame header. */
                        return 0;
                }

                /* Initialize reader */
                rd_slice_init(&rkbuf->rkbuf_reader, &rkbuf->rkbuf_buf, 0, 4);

                /* Reader header: payload length */
                rd_kafka_buf_read_i32(rkbuf, &frame_len);

                if (frame_len < 0 ||
                    frame_len > rktrans->rktrans_rkb->rkb_rk->rk_conf
                                    .recv_max_msg_size) {
                        rd_snprintf(errstr, errstr_size,
                                    "Invalid frame size %" PRId32, frame_len);
                        return -1;
                }

                rkbuf->rkbuf_totlen = 4 + frame_len;
                if (frame_len == 0) {
                        /* Payload is empty, we're done. */
                        rktrans->rktrans_recv_buf = NULL;
                        *rkbufp                   = rkbuf;
                        return 1;
                }

                /* Allocate memory to hold entire frame payload in contigious
                 * memory. */
                rd_buf_write_ensure_contig(&rkbuf->rkbuf_buf, frame_len);

                /* Try reading directly, there is probably more data available*/
                return rd_kafka_transport_framed_recv(rktrans, rkbufp, errstr,
                                                      errstr_size);
        }

        if (rd_buf_write_pos(&rkbuf->rkbuf_buf) == rkbuf->rkbuf_totlen) {
                /* Payload is complete. */
                rktrans->rktrans_recv_buf = NULL;
                *rkbufp                   = rkbuf;
                return 1;
        }

        /* Wait for more data */
        return 0;

err_parse:
        rd_snprintf(errstr, errstr_size, "Frame header parsing failed: %s",
                    rd_kafka_err2str(rkbuf->rkbuf_err));
        return -1;
}


/**
 * @brief Final socket setup after a connection has been established
 */
void rd_kafka_transport_post_connect_setup(rd_kafka_transport_t *rktrans) {
        rd_kafka_broker_t *rkb = rktrans->rktrans_rkb;
        unsigned int slen;

        /* Set socket send & receive buffer sizes if configuerd */
        if (rkb->rkb_rk->rk_conf.socket_sndbuf_size != 0) {
                if (setsockopt(
                        rktrans->rktrans_s, SOL_SOCKET, SO_SNDBUF,
                        (void *)&rkb->rkb_rk->rk_conf.socket_sndbuf_size,
                        sizeof(rkb->rkb_rk->rk_conf.socket_sndbuf_size)) ==
                    RD_SOCKET_ERROR)
                        rd_rkb_log(rkb, LOG_WARNING, "SNDBUF",
                                   "Failed to set socket send "
                                   "buffer size to %i: %s",
                                   rkb->rkb_rk->rk_conf.socket_sndbuf_size,
                                   rd_socket_strerror(rd_socket_errno));
        }

        if (rkb->rkb_rk->rk_conf.socket_rcvbuf_size != 0) {
                if (setsockopt(
                        rktrans->rktrans_s, SOL_SOCKET, SO_RCVBUF,
                        (void *)&rkb->rkb_rk->rk_conf.socket_rcvbuf_size,
                        sizeof(rkb->rkb_rk->rk_conf.socket_rcvbuf_size)) ==
                    RD_SOCKET_ERROR)
                        rd_rkb_log(rkb, LOG_WARNING, "RCVBUF",
                                   "Failed to set socket receive "
                                   "buffer size to %i: %s",
                                   rkb->rkb_rk->rk_conf.socket_rcvbuf_size,
                                   rd_socket_strerror(rd_socket_errno));
        }

        /* Get send and receive buffer sizes to allow limiting
         * the total number of bytes passed with iovecs to sendmsg()
         * and recvmsg(). */
        slen = sizeof(rktrans->rktrans_rcvbuf_size);
        if (getsockopt(rktrans->rktrans_s, SOL_SOCKET, SO_RCVBUF,
                       (void *)&rktrans->rktrans_rcvbuf_size,
                       &slen) == RD_SOCKET_ERROR) {
                rd_rkb_log(rkb, LOG_WARNING, "RCVBUF",
                           "Failed to get socket receive "
                           "buffer size: %s: assuming 1MB",
                           rd_socket_strerror(rd_socket_errno));
                rktrans->rktrans_rcvbuf_size = 1024 * 1024;
        } else if (rktrans->rktrans_rcvbuf_size < 1024 * 64)
                rktrans->rktrans_rcvbuf_size =
                    1024 * 64; /* Use at least 64KB */

        slen = sizeof(rktrans->rktrans_sndbuf_size);
        if (getsockopt(rktrans->rktrans_s, SOL_SOCKET, SO_SNDBUF,
                       (void *)&rktrans->rktrans_sndbuf_size,
                       &slen) == RD_SOCKET_ERROR) {
                rd_rkb_log(rkb, LOG_WARNING, "RCVBUF",
                           "Failed to get socket send "
                           "buffer size: %s: assuming 1MB",
                           rd_socket_strerror(rd_socket_errno));
                rktrans->rktrans_sndbuf_size = 1024 * 1024;
        } else if (rktrans->rktrans_sndbuf_size < 1024 * 64)
                rktrans->rktrans_sndbuf_size =
                    1024 * 64; /* Use at least 64KB */


#ifdef TCP_NODELAY
        if (rkb->rkb_rk->rk_conf.socket_nagle_disable) {
                int one = 1;
                if (setsockopt(rktrans->rktrans_s, IPPROTO_TCP, TCP_NODELAY,
                               (void *)&one, sizeof(one)) == RD_SOCKET_ERROR)
                        rd_rkb_log(rkb, LOG_WARNING, "NAGLE",
                                   "Failed to disable Nagle (TCP_NODELAY) "
                                   "on socket: %s",
                                   rd_socket_strerror(rd_socket_errno));
        }
#endif
}


/**
 * TCP connection established.
 * Set up socket options, SSL, etc.
 *
 * Locality: broker thread
 */
static void rd_kafka_transport_connected(rd_kafka_transport_t *rktrans) {
        rd_kafka_broker_t *rkb = rktrans->rktrans_rkb;

        rd_rkb_dbg(
            rkb, BROKER, "CONNECT", "Connected to %s",
            rd_sockaddr2str(rkb->rkb_addr_last,
                            RD_SOCKADDR2STR_F_PORT | RD_SOCKADDR2STR_F_FAMILY));

        rd_kafka_transport_post_connect_setup(rktrans);

#if WITH_SSL
        if (rkb->rkb_proto == RD_KAFKA_PROTO_SSL ||
            rkb->rkb_proto == RD_KAFKA_PROTO_SASL_SSL) {
                char errstr[512];

                rd_kafka_broker_lock(rkb);
                rd_kafka_broker_set_state(rkb,
                                          RD_KAFKA_BROKER_STATE_SSL_HANDSHAKE);
                rd_kafka_broker_unlock(rkb);

                /* Set up SSL connection.
                 * This is also an asynchronous operation so dont
                 * propagate to broker_connect_done() just yet. */
                if (rd_kafka_transport_ssl_connect(rkb, rktrans, errstr,
                                                   sizeof(errstr)) == -1) {
                        rd_kafka_transport_connect_done(rktrans, errstr);
                        return;
                }
                return;
        }
#endif

        /* Propagate connect success */
        rd_kafka_transport_connect_done(rktrans, NULL);
}



/**
 * @brief the kernel SO_ERROR in \p errp for the given transport.
 * @returns 0 if getsockopt() was succesful (and \p and errp can be trusted),
 * else -1 in which case \p errp 's value is undefined.
 */
static int rd_kafka_transport_get_socket_error(rd_kafka_transport_t *rktrans,
                                               int *errp) {
        socklen_t intlen = sizeof(*errp);

        if (getsockopt(rktrans->rktrans_s, SOL_SOCKET, SO_ERROR, (void *)errp,
                       &intlen) == -1) {
                rd_rkb_dbg(rktrans->rktrans_rkb, BROKER, "SO_ERROR",
                           "Failed to get socket error: %s",
                           rd_socket_strerror(rd_socket_errno));
                return -1;
        }

        return 0;
}


/**
 * IO event handler.
 *
 * @param socket_errstr Is an optional (else NULL) error string from the
 *                      socket layer.
 *
 * Locality: broker thread
 */
static void rd_kafka_transport_io_event(rd_kafka_transport_t *rktrans,
                                        int events,
                                        const char *socket_errstr) {
        char errstr[512];
        int r;
        rd_kafka_broker_t *rkb = rktrans->rktrans_rkb;

        switch (rkb->rkb_state) {
        case RD_KAFKA_BROKER_STATE_CONNECT:
                /* Asynchronous connect finished, read status. */
                if (!(events & (POLLOUT | POLLERR | POLLHUP)))
                        return;

                if (socket_errstr)
                        rd_kafka_broker_fail(
                            rkb, LOG_ERR, RD_KAFKA_RESP_ERR__TRANSPORT,
                            "Connect to %s failed: %s",
                            rd_sockaddr2str(rkb->rkb_addr_last,
                                            RD_SOCKADDR2STR_F_PORT |
                                                RD_SOCKADDR2STR_F_FAMILY),
                            socket_errstr);
                else if (rd_kafka_transport_get_socket_error(rktrans, &r) ==
                         -1) {
                        rd_kafka_broker_fail(
                            rkb, LOG_ERR, RD_KAFKA_RESP_ERR__TRANSPORT,
                            "Connect to %s failed: "
                            "unable to get status from "
                            "socket %d: %s",
                            rd_sockaddr2str(rkb->rkb_addr_last,
                                            RD_SOCKADDR2STR_F_PORT |
                                                RD_SOCKADDR2STR_F_FAMILY),
                            rktrans->rktrans_s, rd_strerror(rd_socket_errno));
                } else if (r != 0) {
                        /* Connect failed */
                        rd_snprintf(
                            errstr, sizeof(errstr), "Connect to %s failed: %s",
                            rd_sockaddr2str(rkb->rkb_addr_last,
                                            RD_SOCKADDR2STR_F_PORT |
                                                RD_SOCKADDR2STR_F_FAMILY),
                            rd_strerror(r));

                        rd_kafka_transport_connect_done(rktrans, errstr);
                } else {
                        /* Connect succeeded */
                        rd_kafka_transport_connected(rktrans);
                }
                break;

        case RD_KAFKA_BROKER_STATE_SSL_HANDSHAKE:
#if WITH_SSL
                rd_assert(rktrans->rktrans_ssl);

                /* Currently setting up SSL connection:
                 * perform handshake. */
                r = rd_kafka_transport_ssl_handshake(rktrans);

                if (r == 0 /* handshake still in progress */ &&
                    (events & POLLHUP)) {
                        rd_kafka_broker_conn_closed(
                            rkb, RD_KAFKA_RESP_ERR__TRANSPORT, "Disconnected");
                        return;
                }

#else
                RD_NOTREACHED();
#endif
                break;

        case RD_KAFKA_BROKER_STATE_AUTH_LEGACY:
                /* SASL authentication.
                 * Prior to broker version v1.0.0 this is performed
                 * directly on the socket without Kafka framing. */
                if (rd_kafka_sasl_io_event(rktrans, events, errstr,
                                           sizeof(errstr)) == -1) {
                        rd_kafka_broker_fail(
                            rkb, LOG_ERR, RD_KAFKA_RESP_ERR__AUTHENTICATION,
                            "SASL authentication failure: %s", errstr);
                        return;
                }

                if (events & POLLHUP) {
                        rd_kafka_broker_fail(rkb, LOG_ERR,
                                             RD_KAFKA_RESP_ERR__AUTHENTICATION,
                                             "Disconnected");

                        return;
                }

                break;

        case RD_KAFKA_BROKER_STATE_APIVERSION_QUERY:
        case RD_KAFKA_BROKER_STATE_AUTH_HANDSHAKE:
        case RD_KAFKA_BROKER_STATE_AUTH_REQ:
        case RD_KAFKA_BROKER_STATE_UP:
        case RD_KAFKA_BROKER_STATE_UPDATE:

                if (events & POLLIN) {
                        while (rkb->rkb_state >= RD_KAFKA_BROKER_STATE_UP &&
                               rd_kafka_recv(rkb) > 0)
                                ;

                        /* If connection went down: bail out early */
                        if (rkb->rkb_state == RD_KAFKA_BROKER_STATE_DOWN)
                                return;
                }

                if (events & POLLHUP) {
                        rd_kafka_broker_conn_closed(
                            rkb, RD_KAFKA_RESP_ERR__TRANSPORT, "Disconnected");
                        return;
                }

                if (events & POLLOUT) {
                        while (rd_kafka_send(rkb) > 0)
                                ;
                }
                break;

        case RD_KAFKA_BROKER_STATE_INIT:
        case RD_KAFKA_BROKER_STATE_DOWN:
        case RD_KAFKA_BROKER_STATE_TRY_CONNECT:
        case RD_KAFKA_BROKER_STATE_REAUTH:
                rd_kafka_assert(rkb->rkb_rk, !*"bad state");
        }
}



#ifdef _WIN32
/**
 * @brief Convert WSA FD_.. events to POLL.. events.
 */
static RD_INLINE int rd_kafka_transport_wsa2events(long wevents) {
        int events = 0;

        if (unlikely(wevents == 0))
                return 0;

        if (wevents & FD_READ)
                events |= POLLIN;
        if (wevents & (FD_WRITE | FD_CONNECT))
                events |= POLLOUT;
        if (wevents & FD_CLOSE)
                events |= POLLHUP;

        rd_dassert(events != 0);

        return events;
}

/**
 * @brief Convert POLL.. events to WSA FD_.. events.
 */
static RD_INLINE int rd_kafka_transport_events2wsa(int events,
                                                   rd_bool_t is_connecting) {
        long wevents = FD_CLOSE;

        if (unlikely(is_connecting))
                return wevents | FD_CONNECT;

        if (events & POLLIN)
                wevents |= FD_READ;
        if (events & POLLOUT)
                wevents |= FD_WRITE;

        return wevents;
}


/**
 * @returns the WinSocket events (as POLL.. events) for the broker socket.
 */
static int rd_kafka_transport_get_wsa_events(rd_kafka_transport_t *rktrans) {
        const int try_bits[4 * 2] = {FD_READ_BIT,  POLLIN,         FD_WRITE_BIT,
                                     POLLOUT,      FD_CONNECT_BIT, POLLOUT,
                                     FD_CLOSE_BIT, POLLHUP};
        int r, i;
        WSANETWORKEVENTS netevents;
        int events                = 0;
        const char *socket_errstr = NULL;
        rd_kafka_broker_t *rkb    = rktrans->rktrans_rkb;

        /* Get Socket event */
        r = WSAEnumNetworkEvents(rktrans->rktrans_s, rktrans->rktrans_wsaevent,
                                 &netevents);
        if (unlikely(r == SOCKET_ERROR)) {
                rd_rkb_log(rkb, LOG_ERR, "WSAWAIT",
                           "WSAEnumNetworkEvents() failed: %s",
                           rd_socket_strerror(rd_socket_errno));
                socket_errstr = rd_socket_strerror(rd_socket_errno);
                return POLLHUP | POLLERR;
        }

        /* Get fired events and errors for each event type */
        for (i = 0; i < RD_ARRAYSIZE(try_bits); i += 2) {
                const int bit   = try_bits[i];
                const int event = try_bits[i + 1];

                if (!(netevents.lNetworkEvents & (1 << bit)))
                        continue;

                if (unlikely(netevents.iErrorCode[bit])) {
                        socket_errstr =
                            rd_socket_strerror(netevents.iErrorCode[bit]);
                        events |= POLLHUP;
                } else {
                        events |= event;

                        if (bit == FD_WRITE_BIT) {
                                /* Writing no longer blocked */
                                rktrans->rktrans_blocked = rd_false;
                        }
                }
        }

        return events;
}


/**
 * @brief Win32: Poll transport and \p rkq cond events.
 *
 * @returns the transport socket POLL.. event bits.
 */
static int rd_kafka_transport_io_serve_win32(rd_kafka_transport_t *rktrans,
                                             rd_kafka_q_t *rkq,
                                             int timeout_ms) {
        const DWORD wsaevent_cnt = 3;
        WSAEVENT wsaevents[3]    = {
            rkq->rkq_cond.mEvents[0],  /* rkq: cnd_signal */
            rkq->rkq_cond.mEvents[1],  /* rkq: cnd_broadcast */
            rktrans->rktrans_wsaevent, /* socket */
        };
        DWORD r;
        int events               = 0;
        rd_kafka_broker_t *rkb   = rktrans->rktrans_rkb;
        rd_bool_t set_pollout    = rd_false;
        rd_bool_t cnd_is_waiting = rd_false;

        /* WSA only sets FD_WRITE (e.g., POLLOUT) when the socket was
         * previously blocked, unlike BSD sockets that set POLLOUT as long as
         * the socket isn't blocked. So we need to imitate the BSD behaviour
         * here and cut the timeout short if a write is wanted and the socket
         * is not currently blocked. */
        if (rktrans->rktrans_rkb->rkb_state != RD_KAFKA_BROKER_STATE_CONNECT &&
            !rktrans->rktrans_blocked &&
            (rktrans->rktrans_pfd[0].events & POLLOUT)) {
                timeout_ms  = 0;
                set_pollout = rd_true;
        } else {
                /* Check if the queue already has ops enqueued in which case we
                 * cut the timeout short. Else add this thread as waiting on the
                 * queue's condvar so that cnd_signal() (et.al.) will perform
                 * SetEvent() and thus wake up this thread in case a new op is
                 * added to the queue. */
                mtx_lock(&rkq->rkq_lock);
                if (rkq->rkq_qlen > 0) {
                        timeout_ms = 0;
                } else {
                        cnd_is_waiting = rd_true;
                        cnd_wait_enter(&rkq->rkq_cond);
                }
                mtx_unlock(&rkq->rkq_lock);
        }

        /* Wait for IO and queue events */
        r = WSAWaitForMultipleEvents(wsaevent_cnt, wsaevents, FALSE, timeout_ms,
                                     FALSE);

        if (cnd_is_waiting) {
                mtx_lock(&rkq->rkq_lock);
                cnd_wait_exit(&rkq->rkq_cond);
                mtx_unlock(&rkq->rkq_lock);
        }

        if (unlikely(r == WSA_WAIT_FAILED)) {
                rd_rkb_log(rkb, LOG_CRIT, "WSAWAIT",
                           "WSAWaitForMultipleEvents failed: %s",
                           rd_socket_strerror(rd_socket_errno));
                return POLLERR;
        } else if (r != WSA_WAIT_TIMEOUT) {
                r -= WSA_WAIT_EVENT_0;

                /* Reset the cond events if any of them were triggered */
                if (r < 2) {
                        ResetEvent(rkq->rkq_cond.mEvents[0]);
                        ResetEvent(rkq->rkq_cond.mEvents[1]);
                }

                /* Get the socket events. */
                events = rd_kafka_transport_get_wsa_events(rktrans);
        }

        /* As explained above we need to set the POLLOUT flag
         * in case it is wanted but not triggered by Winsocket so that
         * io_event() knows it can attempt to send more data. */
        if (likely(set_pollout && !(events & (POLLHUP | POLLERR | POLLOUT))))
                events |= POLLOUT;

        return events;
}
#endif


/**
 * @brief Poll and serve IOs
 *
 * @returns 0 if \p rkq may need additional blocking/timeout polling, else 1.
 *
 * @locality broker thread
 */
int rd_kafka_transport_io_serve(rd_kafka_transport_t *rktrans,
                                rd_kafka_q_t *rkq,
                                int timeout_ms) {
        rd_kafka_broker_t *rkb = rktrans->rktrans_rkb;
        int events;

        rd_kafka_curr_transport = rktrans;

        if (
#ifndef _WIN32
            /* BSD sockets use POLLOUT to indicate success to connect.
             * Windows has its own flag for this (FD_CONNECT). */
            rkb->rkb_state == RD_KAFKA_BROKER_STATE_CONNECT ||
#endif
            (rkb->rkb_state > RD_KAFKA_BROKER_STATE_SSL_HANDSHAKE &&
             rd_kafka_bufq_cnt(&rkb->rkb_waitresps) < rkb->rkb_max_inflight &&
             rd_kafka_bufq_cnt(&rkb->rkb_outbufs) > 0))
                rd_kafka_transport_poll_set(rkb->rkb_transport, POLLOUT);

#ifdef _WIN32
        /* BSD sockets use POLLIN and a following recv() returning 0 to
         * to indicate connection close.
         * Windows has its own flag for this (FD_CLOSE). */
        if (rd_kafka_bufq_cnt(&rkb->rkb_waitresps) > 0)
#endif
                rd_kafka_transport_poll_set(rkb->rkb_transport, POLLIN);

                /* On Windows we can wait for both IO and condvars (rkq)
                 * simultaneously.
                 *
                 * On *nix/BSD sockets we use a local pipe (pfd[1]) to wake
                 * up the rkq. */
#ifdef _WIN32
        events = rd_kafka_transport_io_serve_win32(rktrans, rkq, timeout_ms);

#else
        if (rd_kafka_transport_poll(rktrans, timeout_ms) < 1)
                return 0; /* No events, caller can block on \p rkq poll */

        /* Broker socket events */
        events = rktrans->rktrans_pfd[0].revents;
#endif

        if (events) {
                rd_kafka_transport_poll_clear(rktrans, POLLOUT | POLLIN);

                rd_kafka_transport_io_event(rktrans, events, NULL);
        }

        return 1;
}


/**
 * @brief Create a new transport object using existing socket \p s.
 */
rd_kafka_transport_t *rd_kafka_transport_new(rd_kafka_broker_t *rkb,
                                             rd_socket_t s,
                                             char *errstr,
                                             size_t errstr_size) {
        rd_kafka_transport_t *rktrans;
        int on = 1;
        int r;

#ifdef SO_NOSIGPIPE
        /* Disable SIGPIPE signalling for this socket on OSX */
        if (setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) == -1)
                rd_rkb_dbg(rkb, BROKER, "SOCKET",
                           "Failed to set SO_NOSIGPIPE: %s",
                           rd_socket_strerror(rd_socket_errno));
#endif

#ifdef SO_KEEPALIVE
        /* Enable TCP keep-alives, if configured. */
        if (rkb->rkb_rk->rk_conf.socket_keepalive) {
                if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (void *)&on,
                               sizeof(on)) == RD_SOCKET_ERROR)
                        rd_rkb_dbg(rkb, BROKER, "SOCKET",
                                   "Failed to set SO_KEEPALIVE: %s",
                                   rd_socket_strerror(rd_socket_errno));
        }
#endif

        /* Set the socket to non-blocking */
        if ((r = rd_fd_set_nonblocking(s))) {
                rd_snprintf(errstr, errstr_size,
                            "Failed to set socket non-blocking: %s",
                            rd_socket_strerror(r));
                return NULL;
        }


        rktrans              = rd_calloc(1, sizeof(*rktrans));
        rktrans->rktrans_rkb = rkb;
        rktrans->rktrans_s   = s;

#ifdef _WIN32
        rktrans->rktrans_wsaevent = WSACreateEvent();
        rd_assert(rktrans->rktrans_wsaevent != NULL);
#endif

        return rktrans;
}


/**
 * Initiate asynchronous connection attempt.
 *
 * Locality: broker thread
 */
rd_kafka_transport_t *rd_kafka_transport_connect(rd_kafka_broker_t *rkb,
                                                 const rd_sockaddr_inx_t *sinx,
                                                 char *errstr,
                                                 size_t errstr_size) {
        rd_kafka_transport_t *rktrans;
        int s = -1;
        int r;

        rkb->rkb_addr_last = sinx;

        s = rkb->rkb_rk->rk_conf.socket_cb(sinx->in.sin_family, SOCK_STREAM,
                                           IPPROTO_TCP,
                                           rkb->rkb_rk->rk_conf.opaque);
        if (s == -1) {
                rd_snprintf(errstr, errstr_size, "Failed to create socket: %s",
                            rd_socket_strerror(rd_socket_errno));
                return NULL;
        }

        rktrans = rd_kafka_transport_new(rkb, s, errstr, errstr_size);
        if (!rktrans) {
                rd_kafka_transport_close0(rkb->rkb_rk, s);
                return NULL;
        }

        rd_rkb_dbg(rkb, BROKER, "CONNECT",
                   "Connecting to %s (%s) "
                   "with socket %i",
                   rd_sockaddr2str(sinx, RD_SOCKADDR2STR_F_FAMILY |
                                             RD_SOCKADDR2STR_F_PORT),
                   rd_kafka_secproto_names[rkb->rkb_proto], s);

        /* Connect to broker */
        if (rkb->rkb_rk->rk_conf.connect_cb) {
                rd_kafka_broker_lock(rkb); /* for rkb_nodename */
                r = rkb->rkb_rk->rk_conf.connect_cb(
                    s, (struct sockaddr *)sinx, RD_SOCKADDR_INX_LEN(sinx),
                    rkb->rkb_nodename, rkb->rkb_rk->rk_conf.opaque);
                rd_kafka_broker_unlock(rkb);
        } else {
                if (connect(s, (struct sockaddr *)sinx,
                            RD_SOCKADDR_INX_LEN(sinx)) == RD_SOCKET_ERROR &&
                    (rd_socket_errno != EINPROGRESS
#ifdef _WIN32
                     && rd_socket_errno != WSAEWOULDBLOCK
#endif
                     ))
                        r = rd_socket_errno;
                else
                        r = 0;
        }

        if (r != 0) {
                rd_rkb_dbg(rkb, BROKER, "CONNECT",
                           "Couldn't connect to %s: %s (%i)",
                           rd_sockaddr2str(sinx, RD_SOCKADDR2STR_F_PORT |
                                                     RD_SOCKADDR2STR_F_FAMILY),
                           rd_socket_strerror(r), r);
                rd_snprintf(errstr, errstr_size,
                            "Failed to connect to broker at %s: %s",
                            rd_sockaddr2str(sinx, RD_SOCKADDR2STR_F_NICE),
                            rd_socket_strerror(r));

                rd_kafka_transport_close(rktrans);
                return NULL;
        }

        /* Set up transport handle */
        rktrans->rktrans_pfd[rktrans->rktrans_pfd_cnt++].fd = s;
        if (rkb->rkb_wakeup_fd[0] != -1) {
                rktrans->rktrans_pfd[rktrans->rktrans_pfd_cnt].events = POLLIN;
                rktrans->rktrans_pfd[rktrans->rktrans_pfd_cnt++].fd =
                    rkb->rkb_wakeup_fd[0];
        }


        /* Poll writability to trigger on connection success/failure. */
        rd_kafka_transport_poll_set(rktrans, POLLOUT);

        return rktrans;
}


#ifdef _WIN32
/**
 * @brief Set the WinSocket event poll bit to \p events.
 */
static void rd_kafka_transport_poll_set_wsa(rd_kafka_transport_t *rktrans,
                                            int events) {
        int r;
        r = WSAEventSelect(
            rktrans->rktrans_s, rktrans->rktrans_wsaevent,
            rd_kafka_transport_events2wsa(rktrans->rktrans_pfd[0].events,
                                          rktrans->rktrans_rkb->rkb_state ==
                                              RD_KAFKA_BROKER_STATE_CONNECT));
        if (unlikely(r != 0)) {
                rd_rkb_log(rktrans->rktrans_rkb, LOG_CRIT, "WSAEVENT",
                           "WSAEventSelect() failed: %s",
                           rd_socket_strerror(rd_socket_errno));
        }
}
#endif

void rd_kafka_transport_poll_set(rd_kafka_transport_t *rktrans, int event) {
        if ((rktrans->rktrans_pfd[0].events & event) == event)
                return;

        rktrans->rktrans_pfd[0].events |= event;

#ifdef _WIN32
        rd_kafka_transport_poll_set_wsa(rktrans,
                                        rktrans->rktrans_pfd[0].events);
#endif
}

void rd_kafka_transport_poll_clear(rd_kafka_transport_t *rktrans, int event) {
        if (!(rktrans->rktrans_pfd[0].events & event))
                return;

        rktrans->rktrans_pfd[0].events &= ~event;

#ifdef _WIN32
        rd_kafka_transport_poll_set_wsa(rktrans,
                                        rktrans->rktrans_pfd[0].events);
#endif
}

#ifndef _WIN32
/**
 * @brief Poll transport fds.
 *
 * @returns 1 if an event was raised, else 0, or -1 on error.
 */
static int rd_kafka_transport_poll(rd_kafka_transport_t *rktrans, int tmout) {
        int r;

        r = poll(rktrans->rktrans_pfd, rktrans->rktrans_pfd_cnt, tmout);
        if (r <= 0)
                return r;

        if (rktrans->rktrans_pfd[1].revents & POLLIN) {
                /* Read wake-up fd data and throw away, just used for wake-ups*/
                char buf[1024];
                while (rd_socket_read((int)rktrans->rktrans_pfd[1].fd, buf,
                                      sizeof(buf)) > 0)
                        ; /* Read all buffered signalling bytes */
        }

        return 1;
}
#endif

#ifdef _WIN32
/**
 * @brief A socket write operation would block, flag the socket
 *        as blocked so that POLLOUT events are handled correctly.
 *
 * This is really only used on Windows where POLLOUT (FD_WRITE) is
 * edge-triggered rather than level-triggered.
 */
void rd_kafka_transport_set_blocked(rd_kafka_transport_t *rktrans,
                                    rd_bool_t blocked) {
        rktrans->rktrans_blocked = blocked;
}
#endif


#if 0
/**
 * Global cleanup.
 * This is dangerous and SHOULD NOT be called since it will rip
 * the rug from under the application if it uses any of this functionality
 * in its own code. This means we might leak some memory on exit.
 */
void rd_kafka_transport_term (void) {
#ifdef _WIN32
        (void)WSACleanup(); /* FIXME: dangerous */
#endif
}
#endif

void rd_kafka_transport_init(void) {
#ifdef _WIN32
        WSADATA d;
        (void)WSAStartup(MAKEWORD(2, 2), &d);
#endif
}
