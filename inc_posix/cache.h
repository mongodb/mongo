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
struct __wt_hb;			typedef struct __wt_hb WT_HB;
struct __wt_item;		typedef struct __wt_item WT_ITEM;
struct __wt_item_offp;		typedef struct __wt_item_offp WT_ITEM_OFFP;
struct __wt_item_ovfl;		typedef struct __wt_item_ovfl WT_ITEM_OVFL;
struct __wt_lsn;		typedef struct __wt_lsn WT_LSN;
struct __wt_page;		typedef struct __wt_page WT_PAGE;
struct __wt_page_desc;		typedef struct __wt_page_desc WT_PAGE_DESC;
struct __wt_page_hdr;		typedef struct __wt_page_hdr WT_PAGE_HDR;
struct __wt_page_hqh;		typedef struct __wt_page_hqh WT_PAGE_HQH;
struct __wt_stat;		typedef struct __wt_stat WT_STAT;
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
 * WT_TOC support (the structures are public, so declared in wiredtiger.h).
 *******************************************/
/*
 * We pass around WT_TOCs internally in the Btree, (rather than a DB), because
 * the DB's are free-threaded, and the WT_TOCs are per-thread.  WT_TOCs always
 * reference a DB handle, though.  The API generation number can be separately
 * incremented by calls that spend a lot of time in the library.  For example,
 * a bulk load call will increment the generation number on every loop, when it
 * is no longer pinning any pages.
 */
#define	WT_TOC_API_IGNORE(toc)						\
	(toc)->api_gen = WT_TOC_GEN_IGNORE
#define	WT_TOC_API_RESET(toc)						\
	(toc)->api_gen = (toc)->env->ienv->api_gen
#define	WT_TOC_DB_INIT(toc, _db, _name) do {				\
	WT_TOC_API_RESET(toc);						\
	(toc)->db = (_db);						\
	(toc)->name = (_name);						\
} while (0)
#define	WT_TOC_DB_CLEAR(toc) do {					\
	WT_TOC_API_IGNORE(toc);						\
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

	u_int32_t root_addr;		/* Root address */
	WT_PAGE  *root_page;		/* Root page */

	u_int32_t indx_size_hint;	/* Number of keys on internal pages */

	WT_STATS *stats;		/* Database handle statistics */
	WT_STATS *dstats;		/* Database file statistics */
};

/*******************************************
 * Cache object.
 *******************************************/
struct __wt_hb {
	u_int32_t serialize_private;	/* Private serialization field */
	WT_PAGE *list;			/* Linked list */
};
struct __wt_cache {
	WT_MTX mtx;			/* Cache server mutex */

#define	WT_CACHE_DEFAULT_SIZE	(20)	/* 20MB */
	u_int64_t bytes_max;		/* Maximum bytes */
	u_int64_t bytes_alloc;		/* Allocated bytes */

	/*
	 * !!!
	 * The "private" field needs to be written atomically, and without
	 * overlap, that is, updating it shouldn't cause a read-write-cycle
	 * of the shared field.  Assume a u_int is the correct size for a
	 * single memory bus cycle.
	 */
	u_int private;		/* Cache currently private */
	u_int shared;		/* Cache currently in use (count) */

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
#define	WT_HASH(cache, addr)	((addr) % (cache)->hashsize)
	u_int32_t hashsize;

	WT_HB *hb;

	u_int32_t flags;
};

/*******************************************
 * Environment handle information that doesn't persist.
 *******************************************/
struct __ienv {
	WT_MTX mtx;			/* Global mutex */

	u_int32_t api_gen;		/* API generation number */

	pthread_t workq_tid;		/* workQ thread ID */

	pthread_t cache_tid;		/* Cache thread ID */
	u_int32_t cache_lockout;	/* Cache method lockout */

	TAILQ_HEAD(			/* Locked: TOC list */
	    __wt_toc_qh, __wt_toc) tocqh;
	WT_TOC *toc_add;		/* Locked: TOC to add to list */
	WT_TOC *toc_del;		/* Locked: TOC to delete from list */

	TAILQ_HEAD(
	    __wt_db_qh, __idb) dbqh;	/* Locked: database list */

	TAILQ_HEAD(
	    __wt_fh_qh, __wt_fh) fhqh;	/* Locked: file list */
	u_int next_file_id;		/* Locked: file ID counter */

	WT_CACHE cache;			/* Page cache */
	u_int32_t page_gen;		/* Page cache LRU generation number */

	WT_STATS *stats;		/* Environment handle statistics */

	char *sep;			/* Display separator line */
	char err_buf[32];		/* Last-ditch error buffer */

	u_int32_t flags;
};

#include "extern.h"

#if defined(__cplusplus)
}
#endif
