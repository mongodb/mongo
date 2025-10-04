/*
 * librd - Rapid Development C library
 *
 * Copyright (c) 2012-2022, Magnus Edenhill
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

#ifndef _RDADDR_H_
#define _RDADDR_H_

#ifndef _WIN32
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#else
#define WIN32_MEAN_AND_LEAN
#include <winsock2.h>
#include <ws2ipdef.h>
#endif

#if defined(__FreeBSD__) || defined(_AIX) || defined(__OpenBSD__)
#include <sys/socket.h>
#endif

/**
 * rd_sockaddr_inx_t is a union for either ipv4 or ipv6 sockaddrs.
 * It provides conveniant abstraction of AF_INET* agnostic operations.
 */
typedef union {
        struct sockaddr_in in;
        struct sockaddr_in6 in6;
} rd_sockaddr_inx_t;
#define sinx_family in.sin_family
#define sinx_addr   in.sin_addr
#define RD_SOCKADDR_INX_LEN(sinx)                                              \
        ((sinx)->sinx_family == AF_INET                                        \
             ? sizeof(struct sockaddr_in)                                      \
             : (sinx)->sinx_family == AF_INET6 ? sizeof(struct sockaddr_in6)   \
                                               : sizeof(rd_sockaddr_inx_t))
#define RD_SOCKADDR_INX_PORT(sinx)                                             \
        ((sinx)->sinx_family == AF_INET                                        \
             ? (sinx)->in.sin_port                                             \
             : (sinx)->sinx_family == AF_INET6 ? (sinx)->in6.sin6_port : 0)

#define RD_SOCKADDR_INX_PORT_SET(sinx, port)                                   \
        do {                                                                   \
                if ((sinx)->sinx_family == AF_INET)                            \
                        (sinx)->in.sin_port = port;                            \
                else if ((sinx)->sinx_family == AF_INET6)                      \
                        (sinx)->in6.sin6_port = port;                          \
        } while (0)



/**
 * Returns a thread-local temporary string (may be called up to 32 times
 * without buffer wrapping) containing the human string representation
 * of the sockaddr (which should be AF_INET or AF_INET6 at this point).
 * If the RD_SOCKADDR2STR_F_PORT is provided the port number will be
 * appended to the string.
 * IPv6 address enveloping ("[addr]:port") will also be performed
 * if .._F_PORT is set.
 */
#define RD_SOCKADDR2STR_F_PORT 0x1 /* Append the port. */
#define RD_SOCKADDR2STR_F_RESOLVE                                              \
        0x2                          /* Try to resolve address to hostname.    \
                                      */
#define RD_SOCKADDR2STR_F_FAMILY 0x4 /* Prepend address family. */
#define RD_SOCKADDR2STR_F_NICE       /* Nice and friendly output */            \
        (RD_SOCKADDR2STR_F_PORT | RD_SOCKADDR2STR_F_RESOLVE)
const char *rd_sockaddr2str(const void *addr, int flags);


/**
 * Splits a node:service definition up into their node and svc counterparts
 * suitable for passing to getaddrinfo().
 * Returns NULL on success (and temporarily available pointers in '*node'
 * and '*svc') or error string on failure.
 *
 * Thread-safe but returned buffers in '*node' and '*svc' are only
 * usable until the next call to rd_addrinfo_prepare() in the same thread.
 */
const char *rd_addrinfo_prepare(const char *nodesvc, char **node, char **svc);



typedef struct rd_sockaddr_list_s {
        int rsal_cnt;
        int rsal_curr;
        rd_sockaddr_inx_t rsal_addr[];
} rd_sockaddr_list_t;


/**
 * Returns the next address from a sockaddr list and updates
 * the current-index to point to it.
 *
 * Typical usage is for round-robin connection attempts or similar:
 *   while (1) {
 *       rd_sockaddr_inx_t *sinx = rd_sockaddr_list_next(my_server_list);
 *       if (do_connect((struct sockaddr *)sinx) == -1) {
 *          sleep(1);
 *          continue;
 *       }
 *       ...
 *   }
 *
 */

static RD_INLINE rd_sockaddr_inx_t *
rd_sockaddr_list_next(rd_sockaddr_list_t *rsal) RD_UNUSED;
static RD_INLINE rd_sockaddr_inx_t *
rd_sockaddr_list_next(rd_sockaddr_list_t *rsal) {
        rsal->rsal_curr = (rsal->rsal_curr + 1) % rsal->rsal_cnt;
        return &rsal->rsal_addr[rsal->rsal_curr];
}


#define RD_SOCKADDR_LIST_FOREACH(sinx, rsal)                                   \
        for ((sinx) = &(rsal)->rsal_addr[0];                                   \
             (sinx) < &(rsal)->rsal_addr[(rsal)->rsal_cnt]; (sinx)++)

/**
 * Wrapper for getaddrinfo(3) that performs these additional tasks:
 *  - Input is a combined "<node>[:<svc>]" string, with support for
 *    IPv6 enveloping ("[addr]:port").
 *  - Returns a rd_sockaddr_list_t which must be freed with
 *    rd_sockaddr_list_destroy() when done with it.
 *  - Automatically shuffles the returned address list to provide
 *    round-robin (unless RD_AI_NOSHUFFLE is provided in 'flags').
 *
 * Thread-safe.
 */
#define RD_AI_NOSHUFFLE                                                        \
        0x10000000 /* Dont shuffle returned address list.                      \
                    * FIXME: Guessing non-used bits like this                  \
                    *        is a bad idea. */

struct addrinfo;

rd_sockaddr_list_t *
rd_getaddrinfo(const char *nodesvc,
               const char *defsvc,
               int flags,
               int family,
               int socktype,
               int protocol,
               int (*resolve_cb)(const char *node,
                                 const char *service,
                                 const struct addrinfo *hints,
                                 struct addrinfo **res,
                                 void *opaque),
               void *opaque,
               const char **errstr);



/**
 * Frees a sockaddr list.
 *
 * Thread-safe.
 */
void rd_sockaddr_list_destroy(rd_sockaddr_list_t *rsal);



/**
 * Returns the human readable name of a socket family.
 */
static const char *rd_family2str(int af) RD_UNUSED;
static const char *rd_family2str(int af) {
        switch (af) {
        case AF_INET:
                return "inet";
        case AF_INET6:
                return "inet6";
        default:
                return "af?";
        };
}

#endif /* _RDADDR_H_ */
