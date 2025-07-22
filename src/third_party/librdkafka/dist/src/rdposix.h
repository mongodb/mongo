/*
 * librdkafka - Apache Kafka C library
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

/**
 * POSIX system support
 */
#ifndef _RDPOSIX_H_
#define _RDPOSIX_H_

#include <unistd.h>
#include <stdio.h>
#include <sys/time.h>
#include <inttypes.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

/**
 * Types
 */


/**
 * Annotations, attributes, optimizers
 */
#ifndef likely
#define likely(x) __builtin_expect((x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect((x), 0)
#endif

#define RD_UNUSED             __attribute__((unused))
#define RD_INLINE             inline
#define RD_WARN_UNUSED_RESULT __attribute__((warn_unused_result))
#define RD_NORETURN           __attribute__((noreturn))
#define RD_IS_CONSTANT(p)     __builtin_constant_p((p))
#define RD_TLS                __thread

/**
 * Allocation
 */
#if !defined(__FreeBSD__) && !defined(__OpenBSD__)
/* alloca(3) is in stdlib on FreeBSD */
#include <alloca.h>
#endif

#define rd_alloca(N) alloca(N)


/**
 * Strings, formatting, printf, ..
 */

/* size_t and ssize_t format strings */
#define PRIusz "zu"
#define PRIdsz "zd"

#ifndef RD_FORMAT
#define RD_FORMAT(...) __attribute__((format(__VA_ARGS__)))
#endif
#define rd_snprintf(...)  snprintf(__VA_ARGS__)
#define rd_vsnprintf(...) vsnprintf(__VA_ARGS__)

#define rd_strcasecmp(A, B)     strcasecmp(A, B)
#define rd_strncasecmp(A, B, N) strncasecmp(A, B, N)


#ifdef HAVE_STRCASESTR
#define rd_strcasestr(HAYSTACK, NEEDLE) strcasestr(HAYSTACK, NEEDLE)
#else
#define rd_strcasestr(HAYSTACK, NEEDLE) _rd_strcasestr(HAYSTACK, NEEDLE)
#endif


/**
 * Errors
 */


#define rd_set_errno(err) (errno = (err))

#if HAVE_STRERROR_R
static RD_INLINE RD_UNUSED const char *rd_strerror(int err) {
        static RD_TLS char ret[128];

#if defined(__GLIBC__) && defined(_GNU_SOURCE)
        return strerror_r(err, ret, sizeof(ret));
#else /* XSI version */
        int r;
        /* The r assignment is to catch the case where
         * _GNU_SOURCE is not defined but the GNU version is
         * picked up anyway. */
        r = strerror_r(err, ret, sizeof(ret));
        if (unlikely(r))
                rd_snprintf(ret, sizeof(ret), "strerror_r(%d) failed (ret %d)",
                            err, r);
        return ret;
#endif
}
#else
#define rd_strerror(err) strerror(err)
#endif


/**
 * Atomics
 */
#include "rdatomic.h"

/**
 * Misc
 */

/**
 * Microsecond sleep.
 * Will retry on signal interrupt unless *terminate is true.
 */
static RD_INLINE RD_UNUSED void rd_usleep(int usec, rd_atomic32_t *terminate) {
        struct timespec req = {usec / 1000000, (long)(usec % 1000000) * 1000};

        /* Retry until complete (issue #272), unless terminating. */
        while (nanosleep(&req, &req) == -1 &&
               (errno == EINTR && (!terminate || !rd_atomic32_get(terminate))))
                ;
}



#define rd_gettimeofday(tv, tz) gettimeofday(tv, tz)


#ifndef __COVERITY__
#define rd_assert(EXPR) assert(EXPR)
#else
extern void __coverity_panic__(void);
#define rd_assert(EXPR)                                                        \
        do {                                                                   \
                if (!(EXPR))                                                   \
                        __coverity_panic__();                                  \
        } while (0)
#endif


static RD_INLINE RD_UNUSED const char *rd_getenv(const char *env,
                                                 const char *def) {
        const char *tmp;
        tmp = getenv(env);
        if (tmp && *tmp)
                return tmp;
        return def;
}


/**
 * Empty struct initializer
 */
#define RD_ZERO_INIT                                                           \
        {}

/**
 * Sockets, IO
 */

/** @brief Socket type */
typedef int rd_socket_t;

/** @brief Socket API error return value */
#define RD_SOCKET_ERROR (-1)

/** @brief Last socket error */
#define rd_socket_errno errno


/** @brief String representation of socket error */
#define rd_socket_strerror(ERR) rd_strerror(ERR)

/** @brief poll() struct type */
typedef struct pollfd rd_pollfd_t;

/** @brief poll(2) */
#define rd_socket_poll(POLLFD, FDCNT, TIMEOUT_MS)                              \
        poll(POLLFD, FDCNT, TIMEOUT_MS)

/**
 * @brief Set socket to non-blocking
 * @returns 0 on success or errno on failure.
 */
static RD_UNUSED int rd_fd_set_nonblocking(int fd) {
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl == -1 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) == -1)
                return errno;
        return 0;
}

/**
 * @brief Create non-blocking pipe
 * @returns 0 on success or errno on failure
 */
static RD_UNUSED int rd_pipe_nonblocking(rd_socket_t *fds) {
        if (pipe(fds) == -1 || rd_fd_set_nonblocking(fds[0]) == -1 ||
            rd_fd_set_nonblocking(fds[1]))
                return errno;

                /* Minimize buffer sizes to avoid a large number
                 * of signaling bytes to accumulate when
                 * io-signalled queue is not being served for a while. */
#ifdef F_SETPIPE_SZ
        /* Linux automatically rounds the pipe size up
         * to the minimum size. */
        fcntl(fds[0], F_SETPIPE_SZ, 100);
        fcntl(fds[1], F_SETPIPE_SZ, 100);
#endif
        return 0;
}
#define rd_socket_read(fd, buf, sz)  read(fd, buf, sz)
#define rd_socket_write(fd, buf, sz) write(fd, buf, sz)
#define rd_socket_close(fd)          close(fd)

/* File IO */
#define rd_write(fd, buf, sz)      write(fd, buf, sz)
#define rd_open(path, flags, mode) open(path, flags, mode)
#define rd_close(fd)               close(fd)

#endif /* _RDPOSIX_H_ */
