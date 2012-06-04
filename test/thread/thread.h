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
void load(void);
int  rw_start(u_int, u_int);
void stats(void);
