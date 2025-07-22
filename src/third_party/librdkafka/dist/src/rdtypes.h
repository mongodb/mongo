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

#ifndef _RDTYPES_H_
#define _RDTYPES_H_

#include <inttypes.h>


/*
 * Fundamental types
 */


/* Timestamp (microseconds).
 * Struct members with this type usually have the "ts_" prefix for
 * the internal monotonic clock timestamp, or "wts_" for wall clock timestamp.
 */
typedef int64_t rd_ts_t;

#define RD_TS_MAX INT64_MAX


typedef uint8_t rd_bool_t;
#define rd_true  1
#define rd_false 0


/**
 * @enum Denotes an async or sync operation
 */
typedef enum {
        RD_SYNC = 0, /**< Synchronous/blocking */
        RD_ASYNC,    /**< Asynchronous/non-blocking */
} rd_async_t;


/**
 * @enum Instruct function to acquire or not to acquire a lock
 */
typedef enum {
        RD_DONT_LOCK = 0, /**< Do not acquire lock */
        RD_DO_LOCK   = 1, /**< Do acquire lock */
} rd_dolock_t;


/*
 * Helpers
 */

/**
 * @brief Overflow-safe type-agnostic compare for use in cmp functions.
 *
 * @warning A and B may be evaluated multiple times.
 *
 * @returns -1, 0 or 1.
 */
#define RD_CMP(A, B) (int)((A) < (B) ? -1 : ((A) > (B)))


#endif /* _RDTYPES_H_ */
