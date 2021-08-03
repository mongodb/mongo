/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/* Some systems don't configure 64-bit MIN/MAX by default. */
#ifndef ULLONG_MAX
#define ULLONG_MAX 0xffffffffffffffffULL
#endif
#ifndef LLONG_MAX
#define LLONG_MAX 0x7fffffffffffffffLL
#endif
#ifndef LLONG_MIN
#define LLONG_MIN (-0x7fffffffffffffffLL - 1)
#endif

/* Define O_BINARY for Posix systems */
#define O_BINARY 0

/*
 * Define WT threading and concurrency primitives
 */
typedef pthread_cond_t wt_cond_t;
typedef pthread_mutex_t wt_mutex_t;
typedef struct {
    bool created;
    pthread_t id;
} wt_thread_t;

/*
 * Thread callbacks need to match the platform specific callback types
 */
/* NOLINTNEXTLINE(misc-macro-parentheses) */
#define WT_THREAD_CALLBACK(x) void *(x)
#define WT_THREAD_RET void *
#define WT_THREAD_RET_VALUE NULL

/*
 * WT declaration for calling convention type
 */
#define WT_CDECL
