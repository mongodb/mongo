/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Define WT threading and concurrency primitives Assumes Windows 7+/2008 R2+
 */
typedef CONDITION_VARIABLE wt_cond_t;
typedef CRITICAL_SECTION wt_mutex_t;
typedef struct {
    bool created;
    HANDLE id;
} wt_thread_t;

/*
 * Thread callbacks need to match the return signature of _beginthreadex.
 */
#define WT_THREAD_CALLBACK(x) unsigned(__stdcall x)
#define WT_THREAD_RET unsigned __stdcall
#define WT_THREAD_RET_VALUE 0

/*
 * WT declaration for calling convention type
 */
#define WT_CDECL __cdecl

#if _MSC_VER < 1900
/* Timespec is a POSIX structure not defined in Windows */
struct timespec {
    time_t tv_sec; /* seconds */
    long tv_nsec;  /* nanoseconds */
};
#endif

/*
 * Windows Portability stuff These are POSIX types which Windows lacks Eventually WiredTiger will
 * migrate away from these types
 */
typedef unsigned int u_int;
typedef unsigned char u_char;
typedef unsigned long u_long;

/*
 * Windows does have ssize_t Python headers declare also though so we need to guard it
 */
#ifndef HAVE_SSIZE_T
typedef int ssize_t;
#endif

/* Windows does not provide fsync */
#define fsync _commit
