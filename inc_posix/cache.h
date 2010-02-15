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
struct __wt_cache;		typedef struct __wt_cache WT_CACHE;
struct __wt_fh;			typedef struct __wt_fh WT_FH;
struct __wt_lsn;		typedef struct __wt_lsn WT_LSN;
struct __wt_mtx;		typedef struct __wt_mtx WT_MTX;
struct __wt_page_hqh;		typedef struct __wt_page_hqh WT_PAGE_HQH;
struct __wt_stat;		typedef struct __wt_stat WT_STAT;
struct __wt_stats;		typedef struct __wt_stats WT_STATS;

/*******************************************
 * External include files.
 *******************************************/
#include "queue.h"
#include "bitstring.h"

/*******************************************
 * Internal include files.
 *******************************************/
#include "btree.h"
#include "debug.h"
#include "fh.h"
#include "misc.h"
#include "mutex.h"
#include "stat.h"

/*******************************************
 * WT_TOC support (the structures are public, so declared in wiredtiger.h).
 *******************************************/
/*
 * We pass around WT_TOCs internally in the Btree, (rather than a DB), because
 * the DB's are free-threaded, and the WT_TOCs are per-thread.  WT_TOCs always
 * reference a DB handle, though.
 */
#define	WT_TOC_DB_INIT(toc, _db, _name) do {				\
	(toc)->db = (_db);						\
	(toc)->name = (_name);						\
} while (0)
#define	WT_TOC_DB_CLEAR(toc) do {					\
	(toc)->db = NULL;						\
	(toc)->name = NULL;						\
} while (0)

/*******************************************
 * Cursor handle information that doesn't persist.
 *******************************************/
struct __idbc {
	DBC *dbc;			/* Public object */
};

/*******************************************
 * Database handle information that doesn't persist.
 *******************************************/
struct __idb {
	DB *db;				/* Public object */
	TAILQ_ENTRY(__idb) q;		/* Linked list of databases */

	char	 *dbname;		/* Database name */
	mode_t	  mode;			/* Database file create mode */

	u_int32_t file_id;		/* In-memory file ID */
	WT_FH	 *fh;			/* Backing file handle */

	WT_PAGE  *root_page;		/* Root page */

	u_int32_t indx_size_hint;	/* Number of keys on internal pages */

	void *huffman_key;		/* Key huffman encoding */
	void *huffman_data;		/* Data huffman encoding */

	WT_STATS *stats;		/* Database handle statistics */
	WT_STATS *dstats;		/* Database file statistics */

	u_int32_t flags;
};

/*******************************************
 * Cache support.
 *******************************************/
struct __wt_cache {
	WT_MTX mtx;			/* Cache server mutex */

#define	WT_CACHE_SIZE_DEFAULT	(20)	/* 20MB */

	/*
	 * Each in-memory page is in a hash bucket based on its "address".
	 *
	 * Our hash buckets are very simple list structures.   We depend on
	 * the ability to add/remove an element from the list by writing a
	 * single pointer.  The underlying assumption is that writing a
	 * single pointer will never been seen as a partial write by any
	 * other thread of control, that is, the linked list will always
	 * be consistent.  The end result is that while we have to serialize
	 * the actual manipulation of the memory, we can support multiple
	 * threads of control using the linked lists even while they are
	 * being modified.
	 */
#define	WT_CACHE_HASH_SIZE_DEFAULT	0
#define	WT_HASH(cache, addr)	((addr) % (cache)->hash_size)
	u_int32_t hash_size;

	WT_PAGE **hb;

	u_int32_t flags;
};

typedef struct __wt_drain {
	WT_PAGE	 *page;				/* Page reference */
	u_int32_t gen;				/* Generation */
} WT_DRAIN;

/*******************************************
 * Environment handle information that doesn't persist.
 *******************************************/
struct __ienv {
	WT_MTX mtx;			/* Global mutex */

	pthread_t cache_tid;		/* Cache thread ID */
	pthread_t workq_tid;		/* workQ thread ID */

	TAILQ_HEAD(
	    __wt_db_qh, __idb) dbqh;	/* Locked: database list */

	TAILQ_HEAD(
	    __wt_fh_qh, __wt_fh) fhqh;	/* Locked: file list */
	u_int next_file_id;		/* Locked: file ID counter */

	/*
	 * WiredTiger allocates space for 50 simultaneous threads of control by
	 * default.   The Env.toc_max_set method tunes this if the application
	 * needs more.   Growing the number of threads dynamically is possible,
	 * but tricky since the workQ is walking the array without locking it.
	 *
	 * There's an array of WT_TOC pointers that reference the allocated
	 * array; we do it that way because we want an easy way for the workQ
	 * code to avoid walking the entire array when only a few threads are
	 * running.
	 */
#define	WT_TOC_SIZE_DEFAULT	50
	WT_TOC	**toc;			/* TOC reference */
	u_int32_t toc_cnt;		/* TOC count */
	void	 *toc_array;		/* TOC array */

	/*
	 * WiredTiger allocates space for 10 hazard references in each thread of
	 * control, by default.  The Env.hazard_max_set method tunes this if an
	 * application needs more, but that shouldn't happen, there's no code
	 * path that requires more than 10 pages at a time (and if we find one,
	 * the right change is to increase the default).  The method is there
	 * just in case an application starts failing in the field.
	 *
	 * The hazard array is separate from the WT_TOC array because we want to
	 * be able to easily copy and search it when draining the cache.
	 */
#define	WT_HAZARD_SIZE_DEFAULT	10
	WT_PAGE	**hazard;		/* Hazard references array */

	WT_CACHE  cache;		/* Page cache */
	u_int32_t page_gen;		/* Page cache LRU generation number */

	WT_STATS *stats;		/* Environment handle statistics */

	char *sep;			/* Display separator line */
	char err_buf[32];		/* Last-ditch error buffer */

	u_int32_t flags;
};

#include "serial.h"
#include "extern.h"

#if defined(__cplusplus)
}
#endif
