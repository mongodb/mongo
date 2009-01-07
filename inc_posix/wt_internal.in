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
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*******************************************
 * Internal forward declarations.
 *******************************************/
struct __wt_btree;		typedef struct __wt_btree WT_BTREE;
struct __wt_fh;			typedef struct __wt_fh WT_FH;
struct __wt_global;		typedef struct __wt_global WT_GLOBAL;
struct __wt_item;		typedef struct __wt_item WT_ITEM;
struct __wt_item_offp;		typedef struct __wt_item_offp WT_ITEM_OFFP;
struct __wt_item_ovfl;		typedef struct __wt_item_ovfl WT_ITEM_OVFL;
struct __wt_lsn;		typedef struct __wt_lsn WT_LSN;
struct __wt_page;		typedef struct __wt_page WT_PAGE;
struct __wt_page_desc;		typedef struct __wt_page_desc WT_PAGE_DESC;
struct __wt_page_hdr;		typedef struct __wt_page_hdr WT_PAGE_HDR;
struct __wt_page_hqh;		typedef struct __wt_page_hqh WT_PAGE_HQH;
struct __wt_stat;		typedef struct __wt_stat WT_STAT;

/*******************************************
 * Internal include files.
 *******************************************/
#include "queue.h"			/* External */
#include "bitstring.h"

#include "misc.h"			/* Internal */
#include "fh.h"
#include "global.h"
#include "btree.h"
#include "stat.h"

/*******************************************
 * Database handle information that doesn't persist.
 *******************************************/
struct __idb {
	DB *db;				/* Public object */

	char	 *file_name;		/* Database file name */
	mode_t	  mode;			/* Database file create mode */

	u_int32_t fileid;		/* In-memory file id */
	WT_FH	 *fh;			/* Backing file handle */
	u_int32_t frags;		/* Total fragments in the file */

	u_int32_t root_addr;		/* Root fragment */

	u_int32_t flags;
};

/*******************************************
 * Cursor handle information that doesn't persist.
 *******************************************/
struct __idbc {
	DBC *dbc;			/* Public object */

	u_int32_t flags;
};

/*******************************************
 * Environment handle information that doesn't persist.
 *******************************************/
struct __ienv {
	ENV *env;			/* Public object */

	/*
	 * Cache information.
	 */
#define	WT_CACHE_DEFAULT_SIZE		(20)		/* 20MB */

	/*
	 * Each in-memory page is threaded on two queues: a hash queue
	 * based on its file and page number, and an LRU list.
	 */
	int hashsize;
#define	WT_HASH(ienv, addr)	((addr) % (ienv)->hashsize)
	TAILQ_HEAD(__wt_page_hqh, __wt_page) *hqh;
	TAILQ_HEAD(__wt_page_lqh, __wt_page) lqh;

	/*
	 * The cache is tracked in units of 512B (the minimum frag size), in
	 * 32-bit memory, for a maximum 2TB cache size.  There's no reason
	 * they couldn't be 64-bit types, but there's no need now.
	 */
	u_int32_t cache_frags;		/* Cache fragments allocated */
	u_int32_t cache_frags_max;	/* Cache fragments max */

	u_int32_t flags;
};

#include "extern.h"

#if defined(__cplusplus)
}
#endif
