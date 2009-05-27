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

#define	STOC_PRIME		WT_GLOBAL(sq)
#define	WT_PSTOC_NOT_SET	0
#define	WT_PSTOC_ID		1

struct __wt_stoc {
	u_int16_t id;				/* Server's ID */
	pthread_t tid;				/* System thread ID */

	/*
	 * Per-server thread cache of database pages.
	 */
#define	WT_CACHE_DEFAULT_SIZE		(20)		/* 20MB */

	/*
	 * Each in-memory page is threaded on two queues: a hash queue
	 * based on its file and page number, and an LRU list.
	 */
	u_int32_t hashsize;
#define	WT_HASH(stoc, addr)	((addr) % (stoc)->hashsize)
	TAILQ_HEAD(__wt_page_hqh, __wt_page) *hqh;
	TAILQ_HEAD(__wt_page_lqh, __wt_page) lqh;

	u_int64_t cache_bytes;		/* Cache bytes allocated */
	u_int64_t cache_bytes_max;	/* Cache bytes maximum */
};

struct __wt_workq {
	u_int16_t sid;				/* Server ID */

	WT_TOC *toc;				/* Queued operation */
};

#if defined(__cplusplus)
}
#endif
