/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.  All rights reserved.
 *
 * $Id$
 */

#if defined(__cplusplus)
extern "C" {
#endif

/*******************************************
 * WiredTiger public include file, and configuration control.
 *******************************************/
#include "wiredtiger.h"
#include "wiredtiger_config.h"

/*******************************************
 * WiredTiger system include files.
 *******************************************/
#include <sys/stat.h>
#include <sys/uio.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*******************************************
 * Internal forward declarations.
 *******************************************/
struct __wt_btree;		typedef struct __wt_btree WT_BTREE;
struct __wt_fh;			typedef struct __wt_fh WT_FH;
struct __wt_item;		typedef struct __wt_item WT_ITEM;
struct __wt_item_offp;		typedef struct __wt_item_offp WT_ITEM_OFFP;
struct __wt_item_ovfl;		typedef struct __wt_item_ovfl WT_ITEM_OVFL;
struct __wt_lsn;		typedef struct __wt_lsn WT_LSN;
struct __wt_page;		typedef struct __wt_page WT_PAGE;
struct __wt_page_desc;		typedef struct __wt_page_desc WT_PAGE_DESC;
struct __wt_page_hdr;		typedef struct __wt_page_hdr WT_PAGE_HDR;
struct __wt_page_hqh;		typedef struct __wt_page_hqh WT_PAGE_HQH;
struct __wt_stat;		typedef struct __wt_stat WT_STAT;
struct __wt_srvr;		typedef struct __wt_srvr WT_SRVR;
struct __wt_workq;		typedef struct __wt_workq WT_WORKQ;

/*******************************************
 * Internal include files.
 *******************************************/
#include "queue.h"			/* External */
#include "bitstring.h"

#include "api.h"			/* Internal */
#include "misc.h"
#include "mutex.h"
#include "fh.h"
#include "btree.h"
#include "stat.h"

/*******************************************
 * Cache object.
 *******************************************/
struct __wt_cache {
#define	WT_CACHE_DEFAULT_SIZE		(20)	/* 20MB */
	u_int64_t cache_max;			/* Cache bytes maximum */
	u_int64_t cache_bytes;			/* Cache bytes allocated */

	/*
	 * Each in-memory page is threaded on two queues: a hash queue
	 * based on its file and page number, and an LRU list.
	 */
	u_int32_t hashsize;
#define	WT_HASH(cache, addr)	((addr) % (cache)->hashsize)
	TAILQ_HEAD(__wt_page_hqh, __wt_page) *hqh;
	TAILQ_HEAD(__wt_page_lqh, __wt_page) lqh;

	u_int32_t flags;
};

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

#define	WT_SRVR_SELECT(toc)						\
	((toc)->srvr == WT_SRVR_PRIMARY ?				\
	    &(toc)->env->ienv->psrvr : (toc)->db->idb->srvrq + (toc->srvr - 1))

/*******************************************
 * Cursor handle information that doesn't persist.
 *******************************************/
struct __idbc {
	DBC *dbc;			/* Public object */

	u_int32_t flags;
};

/*******************************************
 * Database handle information that doesn't persist.
 *******************************************/
struct __idb {
	DB *db;				/* Public object */

	char	 *dbname;		/* Database name */
	mode_t	  mode;			/* Database file create mode */

	TAILQ_ENTRY(__idb) q;		/* Linked list of databases */

	u_int32_t file_id;		/* In-memory file ID */
	WT_FH	 *fh;			/* Backing file handle */

	u_int32_t root_addr;		/* Root address */
	WT_PAGE  *root_page;		/* Root page */

	u_int32_t indx_size_hint;	/* Number of keys on internal pages */

	DBT	  key, data;		/* Returned key/data pairs */

	/* Database servers. */
#define	WT_SRVR_FOREACH(idb, srvr, i)					\
	for ((i) = 0, (srvr) = (idb)->srvrq;				\
	    (i) < (idb)->srvrq_entries; ++(i), ++(srvr))
#define	WT_SRVR_SRVRQ_SIZE	64
	WT_SRVR *srvrq;			/* Server thread queue */
	u_int srvrq_entries;		/* Total server entries */

	WT_CACHE *cache;		/* Primary server's database cache */

	WT_STATS *stats;		/* Database handle statistics */
	WT_STATS *dstats;		/* Database file statistics */

	u_int32_t flags;
};

/*******************************************
 * Environment handle information that doesn't persist.
 *******************************************/
struct __ienv {
	ENV *env;			/* Public object */

	WT_MTX mtx;			/* Global mutex */

					/* Locked: list of databases */
	TAILQ_HEAD(__wt_db_qh, __idb) dbqh;
	u_int next_file_id;		/* Locked: serial file ID */

	u_int next_toc_srvr_slot;	/* Locked: next server TOC array slot */

	WT_SRVR psrvr;			/* Primary server */

	WT_STATS *stats;		/* Environment handle statistics */

	char *sep;			/* Display separator line */
	char err_buf[32];		/* Last-ditch error buffer */

	u_int32_t flags;
};

#include "extern.h"

#if defined(__cplusplus)
}
#endif
