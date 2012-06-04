/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#if defined(__cplusplus)
extern "C" {
#endif

/*******************************************
 * WiredTiger public include file, and configuration control.
 *******************************************/
#include "wiredtiger_config.h"
#include "wiredtiger_ext.h"

/*******************************************
 * WiredTiger system include files.
 *******************************************/
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*******************************************
 * WiredTiger externally maintained include files.
 *******************************************/
#include "queue.h"

/*******************************************
 * Forward structure declarations for internal structures.
 *******************************************/
/*
 * DO NOT EDIT: automatically built by dist/s_typedef.
 * Forward structure declarations for internal structures: BEGIN
 */
struct __wt_addr;
    typedef struct __wt_addr WT_ADDR;
struct __wt_block;
    typedef struct __wt_block WT_BLOCK;
struct __wt_block_desc;
    typedef struct __wt_block_desc WT_BLOCK_DESC;
struct __wt_block_header;
    typedef struct __wt_block_header WT_BLOCK_HEADER;
struct __wt_block_snapshot;
    typedef struct __wt_block_snapshot WT_BLOCK_SNAPSHOT;
struct __wt_btree;
    typedef struct __wt_btree WT_BTREE;
struct __wt_btree_session;
    typedef struct __wt_btree_session WT_BTREE_SESSION;
struct __wt_btree_stats;
    typedef struct __wt_btree_stats WT_BTREE_STATS;
struct __wt_cache;
    typedef struct __wt_cache WT_CACHE;
struct __wt_cell;
    typedef struct __wt_cell WT_CELL;
struct __wt_cell_unpack;
    typedef struct __wt_cell_unpack WT_CELL_UNPACK;
struct __wt_col;
    typedef struct __wt_col WT_COL;
struct __wt_col_rle;
    typedef struct __wt_col_rle WT_COL_RLE;
struct __wt_condvar;
    typedef struct __wt_condvar WT_CONDVAR;
struct __wt_config;
    typedef struct __wt_config WT_CONFIG;
struct __wt_config_item;
    typedef struct __wt_config_item WT_CONFIG_ITEM;
struct __wt_connection_impl;
    typedef struct __wt_connection_impl WT_CONNECTION_IMPL;
struct __wt_connection_stats;
    typedef struct __wt_connection_stats WT_CONNECTION_STATS;
struct __wt_cursor_btree;
    typedef struct __wt_cursor_btree WT_CURSOR_BTREE;
struct __wt_cursor_bulk;
    typedef struct __wt_cursor_bulk WT_CURSOR_BULK;
struct __wt_cursor_config;
    typedef struct __wt_cursor_config WT_CURSOR_CONFIG;
struct __wt_cursor_dump;
    typedef struct __wt_cursor_dump WT_CURSOR_DUMP;
struct __wt_cursor_index;
    typedef struct __wt_cursor_index WT_CURSOR_INDEX;
struct __wt_cursor_stat;
    typedef struct __wt_cursor_stat WT_CURSOR_STAT;
struct __wt_cursor_table;
    typedef struct __wt_cursor_table WT_CURSOR_TABLE;
struct __wt_dlh;
    typedef struct __wt_dlh WT_DLH;
struct __wt_evict_entry;
    typedef struct __wt_evict_entry WT_EVICT_ENTRY;
struct __wt_ext;
    typedef struct __wt_ext WT_EXT;
struct __wt_extlist;
    typedef struct __wt_extlist WT_EXTLIST;
struct __wt_fh;
    typedef struct __wt_fh WT_FH;
struct __wt_hazard;
    typedef struct __wt_hazard WT_HAZARD;
struct __wt_ikey;
    typedef struct __wt_ikey WT_IKEY;
struct __wt_insert;
    typedef struct __wt_insert WT_INSERT;
struct __wt_insert_head;
    typedef struct __wt_insert_head WT_INSERT_HEAD;
struct __wt_named_collator;
    typedef struct __wt_named_collator WT_NAMED_COLLATOR;
struct __wt_named_compressor;
    typedef struct __wt_named_compressor WT_NAMED_COMPRESSOR;
struct __wt_named_data_source;
    typedef struct __wt_named_data_source WT_NAMED_DATA_SOURCE;
struct __wt_page;
    typedef struct __wt_page WT_PAGE;
struct __wt_page_header;
    typedef struct __wt_page_header WT_PAGE_HEADER;
struct __wt_page_modify;
    typedef struct __wt_page_modify WT_PAGE_MODIFY;
struct __wt_page_track;
    typedef struct __wt_page_track WT_PAGE_TRACK;
struct __wt_process;
    typedef struct __wt_process WT_PROCESS;
struct __wt_ref;
    typedef struct __wt_ref WT_REF;
struct __wt_row;
    typedef struct __wt_row WT_ROW;
struct __wt_rwlock;
    typedef struct __wt_rwlock WT_RWLOCK;
struct __wt_salvage_cookie;
    typedef struct __wt_salvage_cookie WT_SALVAGE_COOKIE;
struct __wt_scratch_track;
    typedef struct __wt_scratch_track WT_SCRATCH_TRACK;
struct __wt_session_impl;
    typedef struct __wt_session_impl WT_SESSION_IMPL;
struct __wt_size;
    typedef struct __wt_size WT_SIZE;
struct __wt_snapshot;
    typedef struct __wt_snapshot WT_SNAPSHOT;
struct __wt_stats;
    typedef struct __wt_stats WT_STATS;
struct __wt_table;
    typedef struct __wt_table WT_TABLE;
struct __wt_txn;
    typedef struct __wt_txn WT_TXN;
struct __wt_txn_global;
    typedef struct __wt_txn_global WT_TXN_GLOBAL;
struct __wt_update;
    typedef struct __wt_update WT_UPDATE;
/*
 * Forward structure declarations for internal structures: END
 * DO NOT EDIT: automatically built by dist/s_typedef.
 */

/*******************************************
 * WiredTiger internal include files.
 *******************************************/
#include "posix.h"
#include "misc.h"
#include "mutex.h"
#include "txn.h"

#include "block.h"
#include "btmem.h"
#include "btree.h"
#include "cache.h"
#include "config.h"
#include "dlh.h"
#include "error.h"
#include "log.h"
#include "os.h"
#include "stat.h"

#include "api.h"
#include "cursor.h"
#include "meta.h"
#include "schema.h"

#include "extern.h"
#include "verify_build.h"

/* Required by cell.i */
#include "intpack.i"
#include "cell.i"

#include "bitstring.i"
#include "btree.i"
#include "cache.i"
#include "column.i"
#include "cursor.i"
#include "log.i"
#include "mutex.i"
#include "packing.i"
#include "serial.i"
#include "serial_funcs.i"
#include "txn.i"

#if defined(__cplusplus)
}
#endif
