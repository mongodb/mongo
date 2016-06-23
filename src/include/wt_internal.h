/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#ifndef __WT_INTERNAL_H
#define	__WT_INTERNAL_H

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
#ifndef _WIN32
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#endif
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#ifdef _WIN32
#include <io.h>
#endif
#include <limits.h>
#ifdef _WIN32
#include <process.h>
#else
#include <pthread.h>
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <time.h>
#ifdef _WIN32
#define	WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/*
 * DO NOT EDIT: automatically built by dist/s_typedef.
 * Forward type declarations for internal types: BEGIN
 */
struct __wt_addr;
    typedef struct __wt_addr WT_ADDR;
struct __wt_async;
    typedef struct __wt_async WT_ASYNC;
struct __wt_async_cursor;
    typedef struct __wt_async_cursor WT_ASYNC_CURSOR;
struct __wt_async_format;
    typedef struct __wt_async_format WT_ASYNC_FORMAT;
struct __wt_async_op_impl;
    typedef struct __wt_async_op_impl WT_ASYNC_OP_IMPL;
struct __wt_async_worker_state;
    typedef struct __wt_async_worker_state WT_ASYNC_WORKER_STATE;
struct __wt_block;
    typedef struct __wt_block WT_BLOCK;
struct __wt_block_ckpt;
    typedef struct __wt_block_ckpt WT_BLOCK_CKPT;
struct __wt_block_desc;
    typedef struct __wt_block_desc WT_BLOCK_DESC;
struct __wt_block_header;
    typedef struct __wt_block_header WT_BLOCK_HEADER;
struct __wt_bloom;
    typedef struct __wt_bloom WT_BLOOM;
struct __wt_bloom_hash;
    typedef struct __wt_bloom_hash WT_BLOOM_HASH;
struct __wt_bm;
    typedef struct __wt_bm WT_BM;
struct __wt_btree;
    typedef struct __wt_btree WT_BTREE;
struct __wt_cache;
    typedef struct __wt_cache WT_CACHE;
struct __wt_cache_pool;
    typedef struct __wt_cache_pool WT_CACHE_POOL;
struct __wt_cell;
    typedef struct __wt_cell WT_CELL;
struct __wt_cell_unpack;
    typedef struct __wt_cell_unpack WT_CELL_UNPACK;
struct __wt_ckpt;
    typedef struct __wt_ckpt WT_CKPT;
struct __wt_col;
    typedef struct __wt_col WT_COL;
struct __wt_col_rle;
    typedef struct __wt_col_rle WT_COL_RLE;
struct __wt_colgroup;
    typedef struct __wt_colgroup WT_COLGROUP;
struct __wt_compact;
    typedef struct __wt_compact WT_COMPACT;
struct __wt_condvar;
    typedef struct __wt_condvar WT_CONDVAR;
struct __wt_config;
    typedef struct __wt_config WT_CONFIG;
struct __wt_config_check;
    typedef struct __wt_config_check WT_CONFIG_CHECK;
struct __wt_config_entry;
    typedef struct __wt_config_entry WT_CONFIG_ENTRY;
struct __wt_config_parser_impl;
    typedef struct __wt_config_parser_impl WT_CONFIG_PARSER_IMPL;
struct __wt_connection_impl;
    typedef struct __wt_connection_impl WT_CONNECTION_IMPL;
struct __wt_connection_stats;
    typedef struct __wt_connection_stats WT_CONNECTION_STATS;
struct __wt_cursor_backup;
    typedef struct __wt_cursor_backup WT_CURSOR_BACKUP;
struct __wt_cursor_backup_entry;
    typedef struct __wt_cursor_backup_entry WT_CURSOR_BACKUP_ENTRY;
struct __wt_cursor_btree;
    typedef struct __wt_cursor_btree WT_CURSOR_BTREE;
struct __wt_cursor_bulk;
    typedef struct __wt_cursor_bulk WT_CURSOR_BULK;
struct __wt_cursor_config;
    typedef struct __wt_cursor_config WT_CURSOR_CONFIG;
struct __wt_cursor_data_source;
    typedef struct __wt_cursor_data_source WT_CURSOR_DATA_SOURCE;
struct __wt_cursor_dump;
    typedef struct __wt_cursor_dump WT_CURSOR_DUMP;
struct __wt_cursor_index;
    typedef struct __wt_cursor_index WT_CURSOR_INDEX;
struct __wt_cursor_join;
    typedef struct __wt_cursor_join WT_CURSOR_JOIN;
struct __wt_cursor_join_endpoint;
    typedef struct __wt_cursor_join_endpoint WT_CURSOR_JOIN_ENDPOINT;
struct __wt_cursor_join_entry;
    typedef struct __wt_cursor_join_entry WT_CURSOR_JOIN_ENTRY;
struct __wt_cursor_join_iter;
    typedef struct __wt_cursor_join_iter WT_CURSOR_JOIN_ITER;
struct __wt_cursor_json;
    typedef struct __wt_cursor_json WT_CURSOR_JSON;
struct __wt_cursor_log;
    typedef struct __wt_cursor_log WT_CURSOR_LOG;
struct __wt_cursor_lsm;
    typedef struct __wt_cursor_lsm WT_CURSOR_LSM;
struct __wt_cursor_metadata;
    typedef struct __wt_cursor_metadata WT_CURSOR_METADATA;
struct __wt_cursor_stat;
    typedef struct __wt_cursor_stat WT_CURSOR_STAT;
struct __wt_cursor_table;
    typedef struct __wt_cursor_table WT_CURSOR_TABLE;
struct __wt_data_handle;
    typedef struct __wt_data_handle WT_DATA_HANDLE;
struct __wt_data_handle_cache;
    typedef struct __wt_data_handle_cache WT_DATA_HANDLE_CACHE;
struct __wt_dlh;
    typedef struct __wt_dlh WT_DLH;
struct __wt_dsrc_stats;
    typedef struct __wt_dsrc_stats WT_DSRC_STATS;
struct __wt_evict_entry;
    typedef struct __wt_evict_entry WT_EVICT_ENTRY;
struct __wt_evict_queue;
    typedef struct __wt_evict_queue WT_EVICT_QUEUE;
struct __wt_evict_worker;
    typedef struct __wt_evict_worker WT_EVICT_WORKER;
struct __wt_ext;
    typedef struct __wt_ext WT_EXT;
struct __wt_extlist;
    typedef struct __wt_extlist WT_EXTLIST;
struct __wt_fair_lock;
    typedef struct __wt_fair_lock WT_FAIR_LOCK;
struct __wt_fh;
    typedef struct __wt_fh WT_FH;
struct __wt_file_handle_inmem;
    typedef struct __wt_file_handle_inmem WT_FILE_HANDLE_INMEM;
struct __wt_file_handle_posix;
    typedef struct __wt_file_handle_posix WT_FILE_HANDLE_POSIX;
struct __wt_file_handle_win;
    typedef struct __wt_file_handle_win WT_FILE_HANDLE_WIN;
struct __wt_fstream;
    typedef struct __wt_fstream WT_FSTREAM;
struct __wt_hazard;
    typedef struct __wt_hazard WT_HAZARD;
struct __wt_ikey;
    typedef struct __wt_ikey WT_IKEY;
struct __wt_index;
    typedef struct __wt_index WT_INDEX;
struct __wt_insert;
    typedef struct __wt_insert WT_INSERT;
struct __wt_insert_head;
    typedef struct __wt_insert_head WT_INSERT_HEAD;
struct __wt_join_stats;
    typedef struct __wt_join_stats WT_JOIN_STATS;
struct __wt_join_stats_group;
    typedef struct __wt_join_stats_group WT_JOIN_STATS_GROUP;
struct __wt_keyed_encryptor;
    typedef struct __wt_keyed_encryptor WT_KEYED_ENCRYPTOR;
struct __wt_log;
    typedef struct __wt_log WT_LOG;
struct __wt_log_desc;
    typedef struct __wt_log_desc WT_LOG_DESC;
struct __wt_log_op_desc;
    typedef struct __wt_log_op_desc WT_LOG_OP_DESC;
struct __wt_log_rec_desc;
    typedef struct __wt_log_rec_desc WT_LOG_REC_DESC;
struct __wt_log_record;
    typedef struct __wt_log_record WT_LOG_RECORD;
struct __wt_logslot;
    typedef struct __wt_logslot WT_LOGSLOT;
struct __wt_lsm_chunk;
    typedef struct __wt_lsm_chunk WT_LSM_CHUNK;
struct __wt_lsm_data_source;
    typedef struct __wt_lsm_data_source WT_LSM_DATA_SOURCE;
struct __wt_lsm_manager;
    typedef struct __wt_lsm_manager WT_LSM_MANAGER;
struct __wt_lsm_tree;
    typedef struct __wt_lsm_tree WT_LSM_TREE;
struct __wt_lsm_work_unit;
    typedef struct __wt_lsm_work_unit WT_LSM_WORK_UNIT;
struct __wt_lsm_worker_args;
    typedef struct __wt_lsm_worker_args WT_LSM_WORKER_ARGS;
struct __wt_lsm_worker_cookie;
    typedef struct __wt_lsm_worker_cookie WT_LSM_WORKER_COOKIE;
struct __wt_multi;
    typedef struct __wt_multi WT_MULTI;
struct __wt_myslot;
    typedef struct __wt_myslot WT_MYSLOT;
struct __wt_named_collator;
    typedef struct __wt_named_collator WT_NAMED_COLLATOR;
struct __wt_named_compressor;
    typedef struct __wt_named_compressor WT_NAMED_COMPRESSOR;
struct __wt_named_data_source;
    typedef struct __wt_named_data_source WT_NAMED_DATA_SOURCE;
struct __wt_named_encryptor;
    typedef struct __wt_named_encryptor WT_NAMED_ENCRYPTOR;
struct __wt_named_extractor;
    typedef struct __wt_named_extractor WT_NAMED_EXTRACTOR;
struct __wt_named_snapshot;
    typedef struct __wt_named_snapshot WT_NAMED_SNAPSHOT;
struct __wt_ovfl_reuse;
    typedef struct __wt_ovfl_reuse WT_OVFL_REUSE;
struct __wt_ovfl_track;
    typedef struct __wt_ovfl_track WT_OVFL_TRACK;
struct __wt_ovfl_txnc;
    typedef struct __wt_ovfl_txnc WT_OVFL_TXNC;
struct __wt_page;
    typedef struct __wt_page WT_PAGE;
struct __wt_page_deleted;
    typedef struct __wt_page_deleted WT_PAGE_DELETED;
struct __wt_page_header;
    typedef struct __wt_page_header WT_PAGE_HEADER;
struct __wt_page_index;
    typedef struct __wt_page_index WT_PAGE_INDEX;
struct __wt_page_modify;
    typedef struct __wt_page_modify WT_PAGE_MODIFY;
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
struct __wt_save_upd;
    typedef struct __wt_save_upd WT_SAVE_UPD;
struct __wt_scratch_track;
    typedef struct __wt_scratch_track WT_SCRATCH_TRACK;
struct __wt_session_impl;
    typedef struct __wt_session_impl WT_SESSION_IMPL;
struct __wt_size;
    typedef struct __wt_size WT_SIZE;
struct __wt_spinlock;
    typedef struct __wt_spinlock WT_SPINLOCK;
struct __wt_split_stash;
    typedef struct __wt_split_stash WT_SPLIT_STASH;
struct __wt_table;
    typedef struct __wt_table WT_TABLE;
struct __wt_txn;
    typedef struct __wt_txn WT_TXN;
struct __wt_txn_global;
    typedef struct __wt_txn_global WT_TXN_GLOBAL;
struct __wt_txn_op;
    typedef struct __wt_txn_op WT_TXN_OP;
struct __wt_txn_state;
    typedef struct __wt_txn_state WT_TXN_STATE;
struct __wt_update;
    typedef struct __wt_update WT_UPDATE;
union __wt_lsn;
    typedef union __wt_lsn WT_LSN;
union __wt_rand_state;
    typedef union __wt_rand_state WT_RAND_STATE;
/*
 * Forward type declarations for internal types: END
 * DO NOT EDIT: automatically built by dist/s_typedef.
 */

/*******************************************
 * WiredTiger internal include files.
 *******************************************/
#if defined(_lint)
#include "lint.h"
#elif defined(__GNUC__)
#include "gcc.h"
#elif defined(_MSC_VER)
#include "msvc.h"
#endif
#include "hardware.h"
#include "swap.h"

#include "queue.h"

#ifdef _WIN32
#include "os_windows.h"
#else
#include "posix.h"
#endif

#include "misc.h"
#include "mutex.h"

#include "stat.h"			/* required by dhandle.h */
#include "dhandle.h"			/* required by btree.h */

#include "api.h"
#include "async.h"
#include "block.h"
#include "bloom.h"
#include "btmem.h"
#include "btree.h"
#include "cache.h"
#include "compact.h"
#include "config.h"
#include "cursor.h"
#include "dlh.h"
#include "error.h"
#include "flags.h"
#include "log.h"
#include "lsm.h"
#include "meta.h"
#include "os.h"
#include "schema.h"
#include "txn.h"

#include "session.h"			/* required by connection.h */
#include "connection.h"

#include "extern.h"
#ifdef _WIN32
#include "extern_win.h"
#else
#include "extern_posix.h"
#endif
#include "verify_build.h"

#include "ctype.i"			/* required by packing.i */
#include "intpack.i"			/* required by cell.i, packing.i */

#include "buf.i"                        /* required by cell.i */
#include "cache.i"			/* required by txn.i */
#include "cell.i"			/* required by btree.i */
#include "mutex.i"			/* required by btree.i */
#include "txn.i"			/* required by btree.i */

#include "bitstring.i"
#include "btree.i"			/* required by cursor.i */
#include "btree_cmp.i"
#include "column.i"
#include "cursor.i"
#include "log.i"
#include "misc.i"
#include "os_fhandle.i"
#include "os_fs.i"
#include "os_fstream.i"
#include "packing.i"
#include "serial.i"

#if defined(__cplusplus)
}
#endif
#endif					/* !__WT_INTERNAL_H */
