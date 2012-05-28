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

#define	UNUSED(v)	(void)(v)		/* Quiet unused var warnings */

extern WT_CONNECTION *conn;			/* WiredTiger connection */

extern u_int nops;				/* Operations per thread */

#if defined (__GNUC__)
void die(const char *, int) __attribute__((noreturn));
#else
void die(const char *, int);
#endif
int  fop_start(u_int);
void file_create(void);
void file_drop(void);
void file_sync(void);
void file_upgrade(void);
void file_verify(void);
