/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 * 	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#ifndef __WT_INTERNAL_H
#define __WT_INTERNAL_H

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
#endif
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/time.h>
#include <sys/uio.h>
#endif
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <float.h>
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
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/*
 * DO NOT EDIT: automatically built by dist/s_typedef.
 * Forward type declarations for internal types: BEGIN
 */
struct __wt_addr;
typedef struct __wt_addr WT_ADDR;
struct __wt_addr_copy;
typedef struct __wt_addr_copy WT_ADDR_COPY;
struct __wt_background_compact;
typedef struct __wt_background_compact WT_BACKGROUND_COMPACT;
struct __wt_background_compact_exclude;
typedef struct __wt_background_compact_exclude WT_BACKGROUND_COMPACT_EXCLUDE;
struct __wt_background_compact_stat;
typedef struct __wt_background_compact_stat WT_BACKGROUND_COMPACT_STAT;
struct __wt_backup_target;
typedef struct __wt_backup_target WT_BACKUP_TARGET;
struct __wt_blkcache;
typedef struct __wt_blkcache WT_BLKCACHE;
struct __wt_blkcache_delta;
typedef struct __wt_blkcache_delta WT_BLKCACHE_DELTA;
struct __wt_blkcache_item;
typedef struct __wt_blkcache_item WT_BLKCACHE_ITEM;
struct __wt_blkincr;
typedef struct __wt_blkincr WT_BLKINCR;
struct __wt_block;
typedef struct __wt_block WT_BLOCK;
struct __wt_block_ckpt;
typedef struct __wt_block_ckpt WT_BLOCK_CKPT;
struct __wt_block_desc;
typedef struct __wt_block_desc WT_BLOCK_DESC;
struct __wt_block_disagg;
typedef struct __wt_block_disagg WT_BLOCK_DISAGG;
struct __wt_block_disagg_address_cookie;
typedef struct __wt_block_disagg_address_cookie WT_BLOCK_DISAGG_ADDRESS_COOKIE;
struct __wt_block_disagg_header;
typedef struct __wt_block_disagg_header WT_BLOCK_DISAGG_HEADER;
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
struct __wt_bucket_storage;
typedef struct __wt_bucket_storage WT_BUCKET_STORAGE;
struct __wt_cache;
typedef struct __wt_cache WT_CACHE;
struct __wt_cache_eviction_controls;
typedef struct __wt_cache_eviction_controls WT_CACHE_EVICTION_CONTROLS;
struct __wt_cache_pool;
typedef struct __wt_cache_pool WT_CACHE_POOL;
struct __wt_capacity;
typedef struct __wt_capacity WT_CAPACITY;
struct __wt_cell;
typedef struct __wt_cell WT_CELL;
struct __wt_cell_unpack_addr;
typedef struct __wt_cell_unpack_addr WT_CELL_UNPACK_ADDR;
struct __wt_cell_unpack_common;
typedef struct __wt_cell_unpack_common WT_CELL_UNPACK_COMMON;
struct __wt_cell_unpack_delta_int;
typedef struct __wt_cell_unpack_delta_int WT_CELL_UNPACK_DELTA_INT;
struct __wt_cell_unpack_delta_leaf;
typedef struct __wt_cell_unpack_delta_leaf WT_CELL_UNPACK_DELTA_LEAF;
struct __wt_cell_unpack_kv;
typedef struct __wt_cell_unpack_kv WT_CELL_UNPACK_KV;
struct __wt_checkpoint_cleanup;
typedef struct __wt_checkpoint_cleanup WT_CHECKPOINT_CLEANUP;
struct __wt_chunkcache;
typedef struct __wt_chunkcache WT_CHUNKCACHE;
struct __wt_chunkcache_bucket;
typedef struct __wt_chunkcache_bucket WT_CHUNKCACHE_BUCKET;
struct __wt_chunkcache_chunk;
typedef struct __wt_chunkcache_chunk WT_CHUNKCACHE_CHUNK;
struct __wt_chunkcache_hashid;
typedef struct __wt_chunkcache_hashid WT_CHUNKCACHE_HASHID;
struct __wt_chunkcache_intermediate_hash;
typedef struct __wt_chunkcache_intermediate_hash WT_CHUNKCACHE_INTERMEDIATE_HASH;
struct __wt_chunkcache_metadata_work_unit;
typedef struct __wt_chunkcache_metadata_work_unit WT_CHUNKCACHE_METADATA_WORK_UNIT;
struct __wt_chunkcache_pinned_list;
typedef struct __wt_chunkcache_pinned_list WT_CHUNKCACHE_PINNED_LIST;
struct __wt_ckpt;
typedef struct __wt_ckpt WT_CKPT;
struct __wt_ckpt_block_mods;
typedef struct __wt_ckpt_block_mods WT_CKPT_BLOCK_MODS;
struct __wt_ckpt_connection;
typedef struct __wt_ckpt_connection WT_CKPT_CONNECTION;
struct __wt_ckpt_session;
typedef struct __wt_ckpt_session WT_CKPT_SESSION;
struct __wt_ckpt_snapshot;
typedef struct __wt_ckpt_snapshot WT_CKPT_SNAPSHOT;
struct __wt_col;
typedef struct __wt_col WT_COL;
struct __wt_col_fix_auxiliary_header;
typedef struct __wt_col_fix_auxiliary_header WT_COL_FIX_AUXILIARY_HEADER;
struct __wt_col_fix_tw;
typedef struct __wt_col_fix_tw WT_COL_FIX_TW;
struct __wt_col_fix_tw_entry;
typedef struct __wt_col_fix_tw_entry WT_COL_FIX_TW_ENTRY;
struct __wt_col_rle;
typedef struct __wt_col_rle WT_COL_RLE;
struct __wt_col_var_repeat;
typedef struct __wt_col_var_repeat WT_COL_VAR_REPEAT;
struct __wt_colgroup;
typedef struct __wt_colgroup WT_COLGROUP;
struct __wt_compact_state;
typedef struct __wt_compact_state WT_COMPACT_STATE;
struct __wt_condvar;
typedef struct __wt_condvar WT_CONDVAR;
struct __wt_conf;
typedef struct __wt_conf WT_CONF;
struct __wt_conf_bind_desc;
typedef struct __wt_conf_bind_desc WT_CONF_BIND_DESC;
struct __wt_conf_bindings;
typedef struct __wt_conf_bindings WT_CONF_BINDINGS;
struct __wt_conf_value;
typedef struct __wt_conf_value WT_CONF_VALUE;
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
struct __wt_cursor_bounds_state;
typedef struct __wt_cursor_bounds_state WT_CURSOR_BOUNDS_STATE;
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
struct __wt_cursor_hs;
typedef struct __wt_cursor_hs WT_CURSOR_HS;
struct __wt_cursor_index;
typedef struct __wt_cursor_index WT_CURSOR_INDEX;
struct __wt_cursor_layered;
typedef struct __wt_cursor_layered WT_CURSOR_LAYERED;
struct __wt_cursor_metadata;
typedef struct __wt_cursor_metadata WT_CURSOR_METADATA;
struct __wt_cursor_prepare_discovered;
typedef struct __wt_cursor_prepare_discovered WT_CURSOR_PREPARE_DISCOVERED;
struct __wt_cursor_stat;
typedef struct __wt_cursor_stat WT_CURSOR_STAT;
struct __wt_cursor_table;
typedef struct __wt_cursor_table WT_CURSOR_TABLE;
struct __wt_cursor_version;
typedef struct __wt_cursor_version WT_CURSOR_VERSION;
struct __wt_data_handle;
typedef struct __wt_data_handle WT_DATA_HANDLE;
struct __wt_data_handle_cache;
typedef struct __wt_data_handle_cache WT_DATA_HANDLE_CACHE;
struct __wt_delta_cell_int;
typedef struct __wt_delta_cell_int WT_DELTA_CELL_INT;
struct __wt_delta_cell_leaf;
typedef struct __wt_delta_cell_leaf WT_DELTA_CELL_LEAF;
struct __wt_disagg_copy_metadata;
typedef struct __wt_disagg_copy_metadata WT_DISAGG_COPY_METADATA;
struct __wt_disaggregated_checkpoint_track;
typedef struct __wt_disaggregated_checkpoint_track WT_DISAGGREGATED_CHECKPOINT_TRACK;
struct __wt_disaggregated_storage;
typedef struct __wt_disaggregated_storage WT_DISAGGREGATED_STORAGE;
struct __wt_dlh;
typedef struct __wt_dlh WT_DLH;
struct __wt_dsrc_stats;
typedef struct __wt_dsrc_stats WT_DSRC_STATS;
struct __wt_error_info;
typedef struct __wt_error_info WT_ERROR_INFO;
struct __wt_evict;
typedef struct __wt_evict WT_EVICT;
struct __wt_evict_timeline;
typedef struct __wt_evict_timeline WT_EVICT_TIMELINE;
struct __wt_ext;
typedef struct __wt_ext WT_EXT;
struct __wt_extlist;
typedef struct __wt_extlist WT_EXTLIST;
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
struct __wt_generation_cookie;
typedef struct __wt_generation_cookie WT_GENERATION_COOKIE;
struct __wt_generation_drain_cookie;
typedef struct __wt_generation_drain_cookie WT_GENERATION_DRAIN_COOKIE;
struct __wt_hash_map;
typedef struct __wt_hash_map WT_HASH_MAP;
struct __wt_hash_map_item;
typedef struct __wt_hash_map_item WT_HASH_MAP_ITEM;
struct __wt_hazard;
typedef struct __wt_hazard WT_HAZARD;
struct __wt_hazard_array;
typedef struct __wt_hazard_array WT_HAZARD_ARRAY;
struct __wt_hazard_cookie;
typedef struct __wt_hazard_cookie WT_HAZARD_COOKIE;
struct __wt_heuristic_controls;
typedef struct __wt_heuristic_controls WT_HEURISTIC_CONTROLS;
struct __wt_ikey;
typedef struct __wt_ikey WT_IKEY;
struct __wt_import_entry;
typedef struct __wt_import_entry WT_IMPORT_ENTRY;
struct __wt_import_list;
typedef struct __wt_import_list WT_IMPORT_LIST;
struct __wt_index;
typedef struct __wt_index WT_INDEX;
struct __wt_insert;
typedef struct __wt_insert WT_INSERT;
struct __wt_insert_head;
typedef struct __wt_insert_head WT_INSERT_HEAD;
struct __wt_json;
typedef struct __wt_json WT_JSON;
struct __wt_keyed_encryptor;
typedef struct __wt_keyed_encryptor WT_KEYED_ENCRYPTOR;
struct __wt_layered_table;
typedef struct __wt_layered_table WT_LAYERED_TABLE;
struct __wt_layered_table_manager;
typedef struct __wt_layered_table_manager WT_LAYERED_TABLE_MANAGER;
struct __wt_layered_table_manager_entry;
typedef struct __wt_layered_table_manager_entry WT_LAYERED_TABLE_MANAGER_ENTRY;
struct __wt_live_restore_fh_meta;
typedef struct __wt_live_restore_fh_meta WT_LIVE_RESTORE_FH_META;
struct __wt_log_manager;
typedef struct __wt_log_manager WT_LOG_MANAGER;
struct __wt_log_record;
typedef struct __wt_log_record WT_LOG_RECORD;
struct __wt_log_thread;
typedef struct __wt_log_thread WT_LOG_THREAD;
struct __wt_multi;
typedef struct __wt_multi WT_MULTI;
struct __wt_name_flag;
typedef struct __wt_name_flag WT_NAME_FLAG;
struct __wt_named_collator;
typedef struct __wt_named_collator WT_NAMED_COLLATOR;
struct __wt_named_compressor;
typedef struct __wt_named_compressor WT_NAMED_COMPRESSOR;
struct __wt_named_data_source;
typedef struct __wt_named_data_source WT_NAMED_DATA_SOURCE;
struct __wt_named_encryptor;
typedef struct __wt_named_encryptor WT_NAMED_ENCRYPTOR;
struct __wt_named_page_log;
typedef struct __wt_named_page_log WT_NAMED_PAGE_LOG;
struct __wt_named_storage_source;
typedef struct __wt_named_storage_source WT_NAMED_STORAGE_SOURCE;
struct __wt_optrack_header;
typedef struct __wt_optrack_header WT_OPTRACK_HEADER;
struct __wt_optrack_record;
typedef struct __wt_optrack_record WT_OPTRACK_RECORD;
struct __wt_ovfl_reuse;
typedef struct __wt_ovfl_reuse WT_OVFL_REUSE;
struct __wt_ovfl_track;
typedef struct __wt_ovfl_track WT_OVFL_TRACK;
struct __wt_page;
typedef struct __wt_page WT_PAGE;
struct __wt_page_block_meta;
typedef struct __wt_page_block_meta WT_PAGE_BLOCK_META;
struct __wt_page_deleted;
typedef struct __wt_page_deleted WT_PAGE_DELETED;
struct __wt_page_delta_config;
typedef struct __wt_page_delta_config WT_PAGE_DELTA_CONFIG;
struct __wt_page_disagg_info;
typedef struct __wt_page_disagg_info WT_PAGE_DISAGG_INFO;
struct __wt_page_header;
typedef struct __wt_page_header WT_PAGE_HEADER;
struct __wt_page_history;
typedef struct __wt_page_history WT_PAGE_HISTORY;
struct __wt_page_history_item;
typedef struct __wt_page_history_item WT_PAGE_HISTORY_ITEM;
struct __wt_page_history_key;
typedef struct __wt_page_history_key WT_PAGE_HISTORY_KEY;
struct __wt_page_index;
typedef struct __wt_page_index WT_PAGE_INDEX;
struct __wt_page_modify;
typedef struct __wt_page_modify WT_PAGE_MODIFY;
struct __wt_page_walk_skip_stats;
typedef struct __wt_page_walk_skip_stats WT_PAGE_WALK_SKIP_STATS;
struct __wt_pending_prepared_item;
typedef struct __wt_pending_prepared_item WT_PENDING_PREPARED_ITEM;
struct __wt_pending_prepared_map;
typedef struct __wt_pending_prepared_map WT_PENDING_PREPARED_MAP;
struct __wt_prefetch;
typedef struct __wt_prefetch WT_PREFETCH;
struct __wt_prefetch_queue_entry;
typedef struct __wt_prefetch_queue_entry WT_PREFETCH_QUEUE_ENTRY;
struct __wt_process;
typedef struct __wt_process WT_PROCESS;
struct __wt_reconcile_stats;
typedef struct __wt_reconcile_stats WT_RECONCILE_STATS;
struct __wt_reconcile_timeline;
typedef struct __wt_reconcile_timeline WT_RECONCILE_TIMELINE;
struct __wt_recovery_timeline;
typedef struct __wt_recovery_timeline WT_RECOVERY_TIMELINE;
struct __wt_ref;
typedef struct __wt_ref WT_REF;
struct __wt_ref_hist;
typedef struct __wt_ref_hist WT_REF_HIST;
struct __wt_rollback_to_stable;
typedef struct __wt_rollback_to_stable WT_ROLLBACK_TO_STABLE;
struct __wt_row;
typedef struct __wt_row WT_ROW;
struct __wt_rts_cookie;
typedef struct __wt_rts_cookie WT_RTS_COOKIE;
struct __wt_rts_work_unit;
typedef struct __wt_rts_work_unit WT_RTS_WORK_UNIT;
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
struct __wt_session_stash;
typedef struct __wt_session_stash WT_SESSION_STASH;
struct __wt_session_stats;
typedef struct __wt_session_stats WT_SESSION_STATS;
struct __wt_shutdown_timeline;
typedef struct __wt_shutdown_timeline WT_SHUTDOWN_TIMELINE;
struct __wt_size;
typedef struct __wt_size WT_SIZE;
struct __wt_spinlock;
typedef struct __wt_spinlock WT_SPINLOCK;
struct __wt_split_page_hist;
typedef struct __wt_split_page_hist WT_SPLIT_PAGE_HIST;
struct __wt_stash;
typedef struct __wt_stash WT_STASH;
struct __wt_sweep_cookie;
typedef struct __wt_sweep_cookie WT_SWEEP_COOKIE;
struct __wt_table;
typedef struct __wt_table WT_TABLE;
struct __wt_thread;
typedef struct __wt_thread WT_THREAD;
struct __wt_thread_check;
typedef struct __wt_thread_check WT_THREAD_CHECK;
struct __wt_thread_group;
typedef struct __wt_thread_group WT_THREAD_GROUP;
struct __wt_tiered;
typedef struct __wt_tiered WT_TIERED;
struct __wt_tiered_object;
typedef struct __wt_tiered_object WT_TIERED_OBJECT;
struct __wt_tiered_tiers;
typedef struct __wt_tiered_tiers WT_TIERED_TIERS;
struct __wt_tiered_tree;
typedef struct __wt_tiered_tree WT_TIERED_TREE;
struct __wt_tiered_work_unit;
typedef struct __wt_tiered_work_unit WT_TIERED_WORK_UNIT;
struct __wt_time_aggregate;
typedef struct __wt_time_aggregate WT_TIME_AGGREGATE;
struct __wt_time_window;
typedef struct __wt_time_window WT_TIME_WINDOW;
struct __wt_truncate_info;
typedef struct __wt_truncate_info WT_TRUNCATE_INFO;
struct __wt_txn;
typedef struct __wt_txn WT_TXN;
struct __wt_txn_global;
typedef struct __wt_txn_global WT_TXN_GLOBAL;
struct __wt_txn_log;
typedef struct __wt_txn_log WT_TXN_LOG;
struct __wt_txn_op;
typedef struct __wt_txn_op WT_TXN_OP;
struct __wt_txn_printlog_args;
typedef struct __wt_txn_printlog_args WT_TXN_PRINTLOG_ARGS;
struct __wt_txn_shared;
typedef struct __wt_txn_shared WT_TXN_SHARED;
struct __wt_txn_snapshot;
typedef struct __wt_txn_snapshot WT_TXN_SNAPSHOT;
struct __wt_update;
typedef struct __wt_update WT_UPDATE;
struct __wt_update_value;
typedef struct __wt_update_value WT_UPDATE_VALUE;
struct __wt_update_vector;
typedef struct __wt_update_vector WT_UPDATE_VECTOR;
struct __wt_verbose_dump_cookie;
typedef struct __wt_verbose_dump_cookie WT_VERBOSE_DUMP_COOKIE;
struct __wt_verbose_message_info;
typedef struct __wt_verbose_message_info WT_VERBOSE_MESSAGE_INFO;
struct __wt_verbose_multi_category;
typedef struct __wt_verbose_multi_category WT_VERBOSE_MULTI_CATEGORY;
struct __wt_verify_info;
typedef struct __wt_verify_info WT_VERIFY_INFO;
struct __wt_version;
typedef struct __wt_version WT_VERSION;
struct __wti_ckpt_handle_stats;
typedef struct __wti_ckpt_handle_stats WTI_CKPT_HANDLE_STATS;
struct __wti_ckpt_progress;
typedef struct __wti_ckpt_progress WTI_CKPT_PROGRESS;
struct __wti_ckpt_thread;
typedef struct __wti_ckpt_thread WTI_CKPT_THREAD;
struct __wti_ckpt_timer;
typedef struct __wti_ckpt_timer WTI_CKPT_TIMER;
struct __wti_cursor_log;
typedef struct __wti_cursor_log WTI_CURSOR_LOG;
struct __wti_delete_hs_upd;
typedef struct __wti_delete_hs_upd WTI_DELETE_HS_UPD;
struct __wti_evict_entry;
typedef struct __wti_evict_entry WTI_EVICT_ENTRY;
struct __wti_evict_queue;
typedef struct __wti_evict_queue WTI_EVICT_QUEUE;
struct __wti_live_restore_file_handle;
typedef struct __wti_live_restore_file_handle WTI_LIVE_RESTORE_FILE_HANDLE;
struct __wti_live_restore_fs;
typedef struct __wti_live_restore_fs WTI_LIVE_RESTORE_FS;
struct __wti_live_restore_fs_layer;
typedef struct __wti_live_restore_fs_layer WTI_LIVE_RESTORE_FS_LAYER;
struct __wti_live_restore_server;
typedef struct __wti_live_restore_server WTI_LIVE_RESTORE_SERVER;
struct __wti_live_restore_work_item;
typedef struct __wti_live_restore_work_item WTI_LIVE_RESTORE_WORK_ITEM;
struct __wti_log;
typedef struct __wti_log WTI_LOG;
struct __wti_log_desc;
typedef struct __wti_log_desc WTI_LOG_DESC;
struct __wti_logslot;
typedef struct __wti_logslot WTI_LOGSLOT;
struct __wti_myslot;
typedef struct __wti_myslot WTI_MYSLOT;
struct __wti_rec_chunk;
typedef struct __wti_rec_chunk WTI_REC_CHUNK;
struct __wti_rec_dictionary;
typedef struct __wti_rec_dictionary WTI_REC_DICTIONARY;
struct __wti_rec_kv;
typedef struct __wti_rec_kv WTI_REC_KV;
struct __wti_reconcile;
typedef struct __wti_reconcile WTI_RECONCILE;
union __wt_lsn;
typedef union __wt_lsn WT_LSN;
union __wt_rand_state;
typedef union __wt_rand_state WT_RAND_STATE;

typedef struct timespec WT_TIMER;
typedef uint64_t wt_timestamp_t;

/*
 * Forward type declarations for internal types: END
 * DO NOT EDIT: automatically built by dist/s_typedef.
 */

/*
 * Clang and gcc use different mechanisms to detect TSan, clang using __has_feature. Consolidate
 * them into a single TSAN_BUILD pre-processor flag.
 */
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define TSAN_BUILD 1
#endif
#endif

#if defined(__SANITIZE_THREAD__)
#define TSAN_BUILD 1
#endif

/*******************************************
 * WiredTiger internal include files.
 *******************************************/
#if defined(__GNUC__)
#include "gcc.h"
#elif defined(_MSC_VER)
#include "msvc.h"
#endif
/*
 * GLIBC 2.26 and later use the openat syscall to implement open. Set this flag so that our strace
 * tests know to expect this.
 */
#ifdef __GLIBC_PREREQ
#if __GLIBC_PREREQ(2, 26)
#define WT_USE_OPENAT 1
#endif
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

#include "stat.h"         /* required by dhandle.h */
#include "dhandle.h"      /* required by btree.h, connection.h */
#include "timestamp.h"    /* required by reconcile.h */
#include "thread_group.h" /* required by rollback_to_stable.h */
#include "verbose.h"      /* required by rollback_to_stable.h */

#include "api.h"
#include "bitstring.h"
#include "block.h"
#include "block_cache.h"
#include "block_chunkcache.h"
#include "bloom.h"
#include "btmem.h"
#include "btree.h"
#include "cache.h"
#include "../evict/evict.h"
#include "capacity.h"
#include "cell.h"
#include "cursor.h" /* required by checkpoint */
#include "../checkpoint/checkpoint.h"
#include "compact.h"
#include "conf_keys.h" /* required by conf.h */
#include "conf.h"
#include "config.h"
#include "dlh.h"
#include "error.h"
#include "futex.h"
#include "generation.h"
#include "hash_map.h"
#include "hazard.h"
#include "json.h"
#include "../live_restore/live_restore.h"
#include "../log/log.h"
#include "meta.h" /* required by block.h */
#include "optrack.h"
#include "os.h"
#include "../reconcile/reconcile.h"
#include "rollback_to_stable.h"
#include "schema.h"
#include "tiered.h"
#include "truncate.h"
#include "txn.h"

#include "session.h" /* required by connection.h */
#include "version.h" /* required by connection.h */
#include "connection.h"

#include "extern.h"
#ifdef _WIN32
#include "extern_win.h"
#else
#include "extern_posix.h"
#ifdef __linux__
#include "extern_linux.h"
#elif __APPLE__
#include "extern_darwin.h"
#endif
#endif
#include "verify_build.h"

#include "cache_inline.h"
#include "../evict/evict_inline.h" /* required by misc_inline.h */
#include "ctype_inline.h"          /* required by packing_inline.h */
#include "intpack_inline.h"        /* required by cell_inline.h, packing_inline.h */
#include "int4bitpack_inline.h"
#include "misc_inline.h" /* required by mutex_inline.h */

#include "generation_inline.h" /* required by txn_inline.h */
#include "buf_inline.h"        /* required by cell_inline.h */
#include "ref_inline.h"        /* required by btree_inline.h */
#include "timestamp_inline.h"  /* required by btree_inline.h */
#include "cell_inline.h"       /* required by btree_inline.h */
#include "mutex_inline.h"      /* required by btree_inline.h */
#include "txn_inline.h"        /* required by btree_inline.h */

#include "bitstring_inline.h"
#include "block_inline.h"
#include "btree_inline.h" /* required by cursor_inline.h */
#include "btree_cmp_inline.h"
#include "column_inline.h"
#include "conf_inline.h"
#include "cursor_inline.h"
#include "../log/log_inline.h"
#include "modify_inline.h"
#include "os_fhandle_inline.h"
#include "os_fs_inline.h"
#include "os_fstream_inline.h"
#include "packing_inline.h"
#include "serial_inline.h"
#include "str_inline.h"
#include "time_inline.h"

#if defined(__cplusplus)
}
#endif
#endif /* !__WT_INTERNAL_H */
