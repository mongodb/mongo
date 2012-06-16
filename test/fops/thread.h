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

#define	UNUSED(v)	(void)(v)		/* Quiet unused var warnings */

extern WT_CONNECTION *conn;			/* WiredTiger connection */

extern u_int nops;				/* Operations per thread */

extern const char *uri;				/* Object */

#if defined (__GNUC__)
void die(const char *, int) __attribute__((noreturn));
#else
void die(const char *, int);
#endif
int  fop_start(u_int);
void obj_checkpoint(void);
void obj_create(void);
void obj_drop(void);
void obj_upgrade(void);
void obj_verify(void);
