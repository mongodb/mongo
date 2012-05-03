/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <sys/types.h>
#include <sys/time.h>

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wiredtiger.h>

#define	FNAME		"file:__wt"		/* File name */
#define	FNAME_STAT	"__stats"		/* File name for statistics */

#define	UNUSED(v)	(void)(v)		/* Quiet unused var warnings */

extern WT_CONNECTION *conn;			/* WiredTiger connection */

typedef enum { FIX, ROW, VAR } __ftype;		/* File type */
extern __ftype ftype;

extern u_int nkeys;				/* Keys to load */
extern u_int nops;				/* Operations per thread */
extern int   session_per_op;			/* New session per operation */

#if defined (__GNUC__)
void die(const char *, int) __attribute__((noreturn));
#else
void die(const char *, int);
#endif
int  fops(u_int);
void file_create(void);
void file_drop(void);
void file_truncate(void);
void load(void);
int  rw(u_int, u_int);
void stats(void);

/*
 * r --
 *	Return a 32-bit pseudo-random number.
 *
 * This is an implementation of George Marsaglia's multiply-with-carry pseudo-
 * random number generator.  Computationally fast, with reasonable randomness
 * properties.
 */
static inline uint32_t
r(void)
{
	static uint32_t m_w = 0, m_z = 0;

	if (m_w == 0) {
		struct timeval t;
		(void)gettimeofday(&t, NULL);
		m_w = (uint32_t)t.tv_sec;
		m_z = (uint32_t)t.tv_usec;
	}

	m_z = 36969 * (m_z & 65535) + (m_z >> 16);
	m_w = 18000 * (m_w & 65535) + (m_w >> 16);
	return (m_z << 16) + (m_w & 65535);
}
