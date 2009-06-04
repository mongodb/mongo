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
	WT_MTX mtx;				/* Global mutex */

	u_int  running;				/* Engine is running */

	u_int single_threaded;			/* Engine is single-threaded */

	WT_STOC *sq;				/* Server thread queue */
	int sq_next;				/* Next server slot */
	int sq_entries;				/* Total server entries */
	int toc_slot;				/* TOC server slot */

	u_int32_t file_id;			/* Serial file ID */

	char *sep;				/* Display separator line */

	char err_buf[32];			/* Last-ditch error buffer */
};

extern WT_GLOBALS __wt_globals;

#define	WT_GLOBAL(v)	__wt_globals.v

#if defined(__cplusplus)
}
#endif
