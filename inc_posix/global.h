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
struct __wt_globals {
	u_int  running;				/* Engine is running. */

	pthread_t tid;				/* Engine thread ID */

	WT_MTX mtx;				/* Global mutex */

	u_int32_t file_id;			/* Serial file ID */

	WT_TOC **workq;				/* Work queue */
	u_int workq_next;			/* Next empty connection slot */
	u_int workq_entries;			/* Total connection entries */

	char *sep;				/* Display separator line */

	char err_buf[32];			/* Last-ditch error buffer */
};

extern WT_GLOBALS __wt_globals;

#define	WT_GLOBAL(v)	__wt_globals.v

#if defined(__cplusplus)
}
#endif
