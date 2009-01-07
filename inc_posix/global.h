/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * Global data.
 */
struct __wt_global {
	char *sep;				/* Display separator line */

	char  err_buf[32];			/* Last-ditch error buffer */

	int   file_id;				/* Serial file ID */
};

extern WT_GLOBAL __wt_globals;

#define	WT_GLOBAL(v)	__wt_globals.v

#if defined(__cplusplus)
}
#endif
