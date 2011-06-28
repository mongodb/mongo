/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Quiet compiler warnings about unused parameters.
 */
#define	WT_UNUSED(var)	(void)(var)

int util_dump(int, char *[]);
int util_dumpfile(int, char *[]);
int util_load(int, char *[]);
int util_printlog(int, char *[]);
int util_salvage(int, char *[]);
int util_stat(int, char *[]);
int util_verify(int, char *[]);

extern const char *home;				/* Home directory */
extern const char *progname;				/* Program name */
extern const char *usage_prefix;			/* Global arguments */
extern int verbose;					/* Verbose flag */

extern WT_EVENT_HANDLER *verbose_handler;
