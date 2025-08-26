/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include "../../../third_party/openldap_liblmdb/lmdb.h"

/*
 * Both PALM and PALM_KV need these, there's no other convenient place for them.
 */
#ifndef WT_THOUSAND
#define WT_THOUSAND 1000
#define WT_MILLION 1000000
#endif

/*
 * PALM flags start at the 16th bit (0x10000u) to avoid conflicts with __wt_page_log_put_args flags.
 */
#define WT_PALM_KV_TOMBSTONE 0x10000u

/*
 * This include file creates a tiny bit of abstraction for the KV database used, in case we want to
 * ever change to a different implementation.
 *
 * At the moment we use LMDB, which is very similar to Berkeley DB. LMDB often uses MDB as a prefix.
 */
typedef struct PALM_KV_ENV {
    MDB_env *lmdb_env;
    MDB_dbi lmdb_globals_dbi;
    MDB_dbi lmdb_tables_dbi;
    MDB_dbi lmdb_pages_dbi;
    MDB_dbi lmdb_ckpt_dbi;
} PALM_KV_ENV;

typedef struct PALM_KV_CONTEXT {
    PALM_KV_ENV *env;
    MDB_txn *lmdb_txn;
    uint64_t last_materialized_lsn;
    uint32_t materialization_delay_us;
} PALM_KV_CONTEXT;

typedef struct PALM_KV_PAGE_MATCHES {
    PALM_KV_CONTEXT *context;

    MDB_cursor *lmdb_cursor;
    size_t size;
    void *data;
    int error;
    bool first;

    uint64_t query_lsn;

    uint64_t table_id;
    uint64_t page_id;
    uint64_t lsn;

    uint64_t backlink_lsn;
    uint64_t base_lsn;
    WT_PAGE_LOG_ENCRYPTION encryption;
    uint32_t flags;
} PALM_KV_PAGE_MATCHES;

int palm_kv_env_create(PALM_KV_ENV **env, uint32_t cache_size_mb);
int palm_kv_env_open(PALM_KV_ENV *env, const char *homedir);
void palm_kv_env_close(PALM_KV_ENV *env);

int palm_kv_begin_transaction(PALM_KV_CONTEXT *context, PALM_KV_ENV *env, bool readonly);
int palm_kv_commit_transaction(PALM_KV_CONTEXT *context);
void palm_kv_rollback_transaction(PALM_KV_CONTEXT *context);

typedef enum PALM_KV_GLOBAL_KEY {
    PALM_KV_GLOBAL_LSN = 0,
} PALM_KV_GLOBAL_KEY;

int palm_kv_put_global(PALM_KV_CONTEXT *context, PALM_KV_GLOBAL_KEY key, uint64_t value);
int palm_kv_get_global(PALM_KV_CONTEXT *context, PALM_KV_GLOBAL_KEY key, uint64_t *valuep);
int palm_kv_get_page_ids(PALM_KV_CONTEXT *context, WT_ITEM *item, uint64_t checkpoint_lsn,
  uint64_t table_id, size_t *size);
int palm_kv_put_page(PALM_KV_CONTEXT *context, uint64_t table_id, uint64_t page_id, uint64_t lsn,
  bool is_delta, uint64_t backlink_lsn, uint64_t base_lsn, const WT_PAGE_LOG_ENCRYPTION *encryption,
  uint32_t flags, const WT_ITEM *buf);
int palm_kv_get_page_matches(PALM_KV_CONTEXT *context, uint64_t table_id, uint64_t page_id,
  uint64_t lsn, PALM_KV_PAGE_MATCHES *matchesp);
bool palm_kv_next_page_match(PALM_KV_PAGE_MATCHES *matches);
int palm_kv_put_checkpoint(PALM_KV_CONTEXT *context, uint64_t checkpoint_lsn,
  uint64_t checkpoint_timestamp, const WT_ITEM *checkpoint_metadata);
int palm_kv_get_last_checkpoint(PALM_KV_CONTEXT *context, uint64_t *checkpoint_lsn,
  uint64_t *checkpoint_timestamp, void **checkpoint_metadata, size_t *checkpoint_metadata_size);
