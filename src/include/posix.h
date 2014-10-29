/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/* Some systems don't configure 64-bit MIN/MAX by default. */
#ifndef	ULLONG_MAX
#define	ULLONG_MAX	0xffffffffffffffffULL
#endif
#ifndef	LLONG_MAX
#define	LLONG_MAX	0x7fffffffffffffffLL
#endif
#ifndef	LLONG_MIN
#define	LLONG_MIN	(-0x7fffffffffffffffLL - 1)
#endif

/* Define O_BINARY for Posix systems */
#define	O_BINARY 	0

/*
 * Define WT threading and concurrency primitives
 */
typedef pthread_cond_t		wt_cond_t;
typedef pthread_mutex_t		wt_mutex_t;
typedef pthread_t		wt_thread_t;

/*
 * !!!
 * Don't touch this structure without understanding the read/write
 * locking functions.
 */
typedef union {			/* Read/write lock */
#ifdef WORDS_BIGENDIAN
	WiredTiger read/write locks require modification for big-endian systems.
#else
	uint64_t u;
	uint32_t us;
	struct {
		uint16_t writers;
		uint16_t readers;
		uint16_t users;
		uint16_t pad;
	} s;
#endif
} wt_rwlock_t;
