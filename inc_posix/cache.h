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
struct __wt_globals;		typedef struct __wt_globals WT_GLOBALS;
struct __wt_item;		typedef struct __wt_item WT_ITEM;
struct __wt_item_offp;		typedef struct __wt_item_offp WT_ITEM_OFFP;
struct __wt_item_ovfl;		typedef struct __wt_item_ovfl WT_ITEM_OVFL;
struct __wt_lsn;		typedef struct __wt_lsn WT_LSN;
struct __wt_page;		typedef struct __wt_page WT_PAGE;
struct __wt_page_desc;		typedef struct __wt_page_desc WT_PAGE_DESC;
struct __wt_page_hdr;		typedef struct __wt_page_hdr WT_PAGE_HDR;
struct __wt_page_hqh;		typedef struct __wt_page_hqh WT_PAGE_HQH;
struct __wt_stat;		typedef struct __wt_stat WT_STAT;
struct __wt_stoc;		typedef struct __wt_stoc WT_STOC;
struct __wt_workq;		typedef struct __wt_workq WT_WORKQ;

/*******************************************
 * Internal include files.
 *******************************************/
#include "queue.h"			/* External */
#include "bitstring.h"

#include "misc.h"			/* Internal */
#include "mutex.h"
#include "fh.h"
#include "global.h"
#include "btree.h"
#include "connect.h"
#include "connect_auto.h"
#include "stat.h"

/*******************************************
 * Database handle information that doesn't persist.
 *******************************************/
struct __idb {
	DB *db;				/* Public object */

	char	 *dbname;		/* Database name */
	mode_t	  mode;			/* Database file create mode */

	WT_STOC *stoc;			/* Server thread */

	u_int32_t file_id;		/* In-memory file ID */
	WT_FH	 *fh;			/* Backing file handle */

	u_int32_t root_addr;		/* Root address */
	WT_PAGE  *root_page;		/* Root page */

	u_int32_t indx_size_hint;	/* Number of keys on internal pages */

	DBT	  key, data;		/* Returned key/data pairs */

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
	WT_TOC *toc;			/* Enclosing thread of control */
	ENV *env;			/* Public object */

	u_int32_t flags;
};

#include "extern.h"

#if defined(__cplusplus)
}
#endif
