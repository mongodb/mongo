/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2009 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#if defined(__cplusplus)
extern "C" {
#endif

#define	WT_SRVR_SELECT(toc)						\
	((toc)->srvr == WT_SRVR_PRIMARY ?				\
	    &(toc)->env->ienv->psrvr : (toc)->db->idb->srvrq + (toc->srvr - 1))

/*******************************************
 * Server thread-of-control information
 *******************************************/
struct __wt_srvr {
	pthread_t tid;			/* System thread ID */

	int running;			/* Thread active */

	/*
	 * The server ID is a negative number for system-wide servers, and
	 * 0 or positive for database servers (and, in the latter case,
	 * doubles as an array offset into the IDB list of database servers.
	 */
#define	WT_SRVR_PRIMARY		-1
#define	WT_IS_PRIMARY(srvr)						\
	((srvr)->id == WT_SRVR_PRIMARY)
	int id;				/* Server ID (array offset) */

	/*
	 * Enclosing environment, set when the WT_SRVR is created (which
	 * implies that servers are confined to an environment).
	 */
	ENV	*env;			/* Server environment */

	WT_CACHE *cache;		/* Database page cache */

#define	WT_SRVR_TOCQ_SIZE	 40	/* Queued operations max */
	WT_TOC *ops[WT_SRVR_TOCQ_SIZE];	/* Queued operations */

	WT_STATS *stats;		/* Server statistics */
};

#if defined(__cplusplus)
}
#endif
