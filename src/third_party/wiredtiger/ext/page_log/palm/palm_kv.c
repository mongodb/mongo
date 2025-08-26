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

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

/*
 * In theory, extensions should not call into WT functions willy-nilly, but the swap functions are
 * inlined. We could call the system's swap functions directly, and/or write our own, but we'd
 * duplicate some existing logic.
 *
 * Possibly some functions like swap should live in a more general library than WT.
 */
#include <gcc.h>
#include <swap.h>

#include "palm_kv.h"
#include "palm_utils.h"
#define MEGABYTE (1024 * 1024)

/*
 * LMDB requires the number of tables to be known at startup. If we add any more tables, we need to
 * increment this.
 */
#define PALM_MAX_DBI 4

/*
 * The PAGE_KEY is the on disk format for the key of the pages table. The value is a set of bytes,
 * representing the raw page bytes or delta bytes.
 */
typedef struct PAGE_KEY {
    uint64_t table_id;
    uint64_t page_id;
    uint64_t lsn;
    uint32_t is_delta;

    /*
     * These are not really things we key on, but they are more convenient to store in the key
     * rather than the data.
     */
    uint64_t backlink_lsn;
    uint64_t base_lsn;
    uint32_t flags;

    /*
     * An encryption key. This module does no encryption, but we do generate a fake key and return
     * it for testing.
     */
    WT_PAGE_LOG_ENCRYPTION encryption;

    /* To simulate materialization delays, this is the timestamp this record becomes available. */
    uint64_t timestamp_materialized_us;

} PAGE_KEY;

/*
 * The CKPT_KEY is the on disk format for the checkpoints.
 */
typedef struct CKPT_KEY {
    uint64_t lsn;

    /*
     * These are not really things we key on, but they are more convenient to store in the key
     * rather than the data.
     */
    uint64_t checkpoint_timestamp;
} CKPT_KEY;

static bool palm_need_swap = true; /* TODO: derive this */

/*
 * Byte swap a page key so that it sorts in the expected order.
 */
static void
swap_page_key(const PAGE_KEY *src, PAGE_KEY *dest)
{
    if (!palm_need_swap)
        return;

    if (dest != src)
        /* Copy all values by default. */
        *dest = *src;

    /*
     * We don't need to swap all the fields in the key, only the ones that we use in comparisons.
     * Other fields in the key that we don't swap are more like data fields, but they are more
     * convenient to keep in the key.
     */
    dest->table_id = __wt_bswap64(src->table_id);
    dest->page_id = __wt_bswap64(src->page_id);
    dest->lsn = __wt_bswap64(src->lsn);
    dest->is_delta = __wt_bswap32(src->is_delta);
}

/*
 * Byte swap a checkpoint key so that it sorts in the expected order.
 */
static void
swap_ckpt_key(const CKPT_KEY *src, CKPT_KEY *dest)
{
    if (!palm_need_swap)
        return;

    if (dest != src)
        /* Copy all values by default. */
        *dest = *src;

    /*
     * We don't need to swap all the fields in the key, only the ones that we use in comparisons.
     * Other fields in the key that we don't swap are more like data fields, but they are more
     * convenient to keep in the key.
     */
    dest->lsn = __wt_bswap64(src->lsn);
}

/*
 * True if and only if the result matches the table, and page and is materialized, and the page's
 * version is LTE the given checkpoint.
 */
#define RESULT_MATCH(result_key, _context, _table_id, _page_id, _lsn, _now)                 \
    ((result_key)->table_id == (_table_id) && (result_key)->page_id == (_page_id) &&        \
      ((result_key)->lsn <= (_lsn)) && (_now) > (result_key)->timestamp_materialized_us) && \
      ((_context)->last_materialized_lsn == 0 || _lsn <= (_context)->last_materialized_lsn)

#ifdef PALM_KV_DEBUG
/* Show the contents of the PAGE_KEY to stderr.  This can be useful for debugging. */
#define SHOW_PAGE_KEY(pk, label)                                                           \
    fprintf(stderr, "  %s:  t=%" PRIu64 ", p=%" PRIu64 ", l=%" PRIu64 ", isd=%d\n", label, \
      pk->table_id, pk->page_id, pk->lsn, (int)pk->is_delta)

/*
 * Return a string representing the current match value. Can only be used in single threaded code!
 */
static const char *
ret_match_string(PALM_KV_PAGE_MATCHES *matches)
{
    u_int i;
    static char return_string[256]; /* Return value. */

    for (i = 0; i < matches->size && i + 1 < sizeof(return_string); ++i)
        return_string[i] = ((char *)matches->data)[i];
    return_string[i] = '\0';
    return (return_string);
}
#endif

static uint64_t
palm_kv_timestamp_us(void)
{
    struct timeval v;
    int ret;

    ret = gettimeofday(&v, NULL);
    assert(ret == 0);
    (void)ret; /* Assure that ret is "used" when assertions are not in effect. */

    return (uint64_t)(v.tv_sec * WT_MILLION + v.tv_usec);
}

int
palm_kv_env_create(PALM_KV_ENV **envp, uint32_t cache_size_mb)
{
    PALM_KV_ENV *env;
    int ret;

    env = (PALM_KV_ENV *)calloc(1, sizeof(PALM_KV_ENV));
    if (env == 0)
        return (ENOMEM);
    if ((ret = mdb_env_create(&env->lmdb_env)) != 0) {
        free(env);
        return (ret);
    }
    if ((ret = mdb_env_set_maxdbs(env->lmdb_env, PALM_MAX_DBI)) != 0) {
        free(env);
        return (ret);
    }
    if ((ret = mdb_env_set_mapsize(env->lmdb_env, cache_size_mb * MEGABYTE)) != 0) {
        free(env);
        return (ret);
    }
    *envp = env;
    return (ret);
}

int
palm_kv_env_open(PALM_KV_ENV *env, const char *homedir)
{
    int dead_count, ret;
    MDB_txn *txn;

    if ((ret = mdb_env_open(env->lmdb_env, homedir, 0, 0666)) != 0)
        return (ret);

    /* For good multi-process hygiene, this should be called periodically. */
    /* TODO: add this call at checkpoints, or every 10000 calls, etc. */
    if ((ret = mdb_reader_check(env->lmdb_env, &dead_count)) != 0)
        return (ret);
    (void)dead_count;

    if ((ret = mdb_txn_begin(env->lmdb_env, NULL, 0, &txn)) != 0)
        return (ret);

    /* Note: if adding a new named database, increase PALM_MAX_DBI. */
    if ((ret = mdb_dbi_open(txn, "globals", MDB_CREATE | MDB_INTEGERKEY, &env->lmdb_globals_dbi)) !=
      0) {
        mdb_txn_abort(txn);
        return (ret);
    }
    if ((ret = mdb_dbi_open(txn, "tables", MDB_CREATE | MDB_INTEGERKEY, &env->lmdb_tables_dbi)) !=
      0) {
        mdb_txn_abort(txn);
        return (ret);
    }
    if ((ret = mdb_dbi_open(txn, "pages", MDB_CREATE, &env->lmdb_pages_dbi)) != 0) {
        mdb_txn_abort(txn);
        return (ret);
    }
    if ((ret = mdb_dbi_open(txn, "checkpoints", MDB_CREATE, &env->lmdb_ckpt_dbi)) != 0) {
        mdb_txn_abort(txn);
        return (ret);
    }
    if ((ret = mdb_txn_commit(txn)) != 0)
        return (ret);

    return (ret);
}

void
palm_kv_env_close(PALM_KV_ENV *env)
{
    (void)mdb_env_close(env->lmdb_env);
    free(env);
}

int
palm_kv_begin_transaction(PALM_KV_CONTEXT *context, PALM_KV_ENV *env, bool readonly)
{
    context->env = env;
    context->lmdb_txn = NULL;
    /* TODO: report failures?  For all these functions */
    return (mdb_txn_begin(env->lmdb_env, NULL, readonly ? MDB_RDONLY : 0, &context->lmdb_txn));
}

int
palm_kv_commit_transaction(PALM_KV_CONTEXT *context)
{
    int ret;

    assert(context->lmdb_txn != NULL);
    ret = mdb_txn_commit(context->lmdb_txn);
    context->lmdb_txn = NULL;
    return (ret);
}

void
palm_kv_rollback_transaction(PALM_KV_CONTEXT *context)
{
    assert(context->lmdb_txn != NULL);
    mdb_txn_abort(context->lmdb_txn);
    context->lmdb_txn = NULL;
}

int
palm_kv_put_global(PALM_KV_CONTEXT *context, PALM_KV_GLOBAL_KEY key, uint64_t value)
{
    MDB_val kval;
    MDB_val vval;
    u_int k;

    assert(context->lmdb_txn != NULL);

    memset(&kval, 0, sizeof(kval));
    memset(&vval, 0, sizeof(kval));

    k = (u_int)key;
    if (value > UINT_MAX)
        return (EINVAL);

    kval.mv_size = sizeof(k);
    kval.mv_data = &k;
    vval.mv_size = sizeof(value);
    vval.mv_data = &value;
    return (mdb_put(context->lmdb_txn, context->env->lmdb_globals_dbi, &kval, &vval, 0));
}

int
palm_kv_get_global(PALM_KV_CONTEXT *context, PALM_KV_GLOBAL_KEY key, uint64_t *valuep)
{
    MDB_val kval;
    MDB_val vval;
    u_int k;
    int ret;

    assert(context->lmdb_txn != NULL);

    memset(&kval, 0, sizeof(kval));
    memset(&vval, 0, sizeof(kval));
    k = (u_int)key;

    kval.mv_size = sizeof(k);
    kval.mv_data = &k;
    ret = mdb_get(context->lmdb_txn, context->env->lmdb_globals_dbi, &kval, &vval);
    if (ret == 0) {
        if (vval.mv_size != sizeof(uint64_t))
            return (EIO); /* not expected, data damaged, could be assert */
        *valuep = *(uint64_t *)vval.mv_data;
    }
    return (ret);
}

int
palm_kv_put_page(PALM_KV_CONTEXT *context, uint64_t table_id, uint64_t page_id, uint64_t lsn,
  bool is_delta, uint64_t backlink_lsn, uint64_t base_lsn, const WT_PAGE_LOG_ENCRYPTION *encryption,
  uint32_t flags, const WT_ITEM *buf)
{
    MDB_val kval;
    MDB_val vval;
    PAGE_KEY page_key;

    memset(&kval, 0, sizeof(kval));
    memset(&vval, 0, sizeof(kval));
    memset(&page_key, 0, sizeof(page_key));
    page_key.table_id = table_id;
    page_key.page_id = page_id;
    page_key.lsn = lsn;
    page_key.is_delta = is_delta;
    page_key.backlink_lsn = backlink_lsn;
    page_key.base_lsn = base_lsn;
    page_key.flags = flags;
    page_key.encryption = *encryption;
    page_key.timestamp_materialized_us = palm_kv_timestamp_us() + context->materialization_delay_us;
    swap_page_key(&page_key, &page_key);
    kval.mv_size = sizeof(page_key);
    kval.mv_data = &page_key;
    vval.mv_size = buf->size;
    vval.mv_data = (void *)buf->data;

    return (mdb_put(context->lmdb_txn, context->env->lmdb_pages_dbi, &kval, &vval, 0));
}

int
palm_kv_get_page_ids(
  PALM_KV_CONTEXT *context, WT_ITEM *item, uint64_t checkpoint_lsn, uint64_t table_id, size_t *size)
{
    MDB_cursor *cursor;
    MDB_stat stat;
    MDB_val kval;
    MDB_val vval;
    PAGE_KEY page_key;
    size_t count = 0;
    int ret;

    cursor = NULL;

    memset(&kval, 0, sizeof(kval));
    memset(&vval, 0, sizeof(vval));
    memset(&page_key, 0, sizeof(page_key));
    page_key.table_id = table_id;
    swap_page_key(&page_key, &page_key);
    kval.mv_size = sizeof(page_key);
    kval.mv_data = &page_key;

    if ((ret = mdb_cursor_open(context->lmdb_txn, context->env->lmdb_pages_dbi, &cursor)) != 0)
        return (ret);

    /* Get the maximum count of page IDs */
    if ((ret = mdb_stat(context->lmdb_txn, context->env->lmdb_pages_dbi, &stat)) != 0)
        return (ret);

    /* If no entries found, return an error.  */
    if (stat.ms_entries == 0) {
        item->size = 0;
        item->data = NULL;
        return (WT_NOTFOUND);
    }

    assert(item != NULL);
    memset(item, 0, sizeof(*item));
    if ((ret = palm_resize_item(item, stat.ms_entries * sizeof(uint64_t))) != 0)
        return (ret);

    if (item->data == NULL)
        return (ENOMEM);

    if ((ret = mdb_cursor_get(cursor, &kval, &vval, MDB_SET_RANGE)) != 0)
        return (ret);

    uint64_t prev_page_id = 0;
    int prev_is_tombstone = 0;

    /*
     * Iterate through the pages table, looking for pages that have an LSN smaller than the given
     * checkpoint LSN. Note that the pages are sorted by table_id, page_id, LSN in ascending order,
     * so we are only interested in the last page of each page_id. If the last page is a tombstone,
     * meaning we're discarding it, then we skip it. If the last page is a full page, we store the
     * page ID in the item->data array.
     */
    while (ret == 0) {
        if (kval.mv_size != sizeof(PAGE_KEY))
            return (EINVAL);

        PAGE_KEY *key = (PAGE_KEY *)kval.mv_data;
        PAGE_KEY decoded_key;
        swap_page_key(key, &decoded_key);

        /* If we have gone past the table, stop. */
        if (decoded_key.table_id > table_id)
            break;

        /*
         * Skip pages that are not for the requested table and pages newer than the given checkpoint
         * LSN. For deltas, skip those that are not tombstones, since only full pages and tombstones
         * are relevant here.
         */
        if (decoded_key.table_id < table_id || decoded_key.lsn >= checkpoint_lsn ||
          (decoded_key.is_delta && ((decoded_key.flags & WT_PALM_KV_TOMBSTONE) == 0))) {
            ret = mdb_cursor_get(cursor, &kval, &vval, MDB_NEXT);
            if (ret != 0 && ret != MDB_NOTFOUND)
                return (ret);
            continue;
        }

        /*  If the previous page was not a tombstone, store the previous page ID. */
        if (prev_page_id != 0 && decoded_key.page_id != prev_page_id && !prev_is_tombstone) {
            assert(count < stat.ms_entries);
            ((uint64_t *)item->data)[count++] = prev_page_id;
        }

        prev_page_id = decoded_key.page_id;
        prev_is_tombstone = ((decoded_key.flags & WT_PALM_KV_TOMBSTONE) != 0);

        ret = mdb_cursor_get(cursor, &kval, &vval, MDB_NEXT);
        if (ret != 0 && ret != MDB_NOTFOUND)
            return (ret);
    }

    /* If the last tracked page was not a tombstone, store the page ID. */
    if (prev_page_id != 0 && !prev_is_tombstone) {
        assert(count < stat.ms_entries);
        ((uint64_t *)item->data)[count++] = prev_page_id;
    }

    *size = count;

    return (ret);
}

int
palm_kv_get_page_matches(PALM_KV_CONTEXT *context, uint64_t table_id, uint64_t page_id,
  uint64_t lsn, PALM_KV_PAGE_MATCHES *matches)
{
    MDB_val kval;
    MDB_val vval;
    PAGE_KEY page_key;
    PAGE_KEY result_key;
    PAGE_KEY *readonly_result_key;
    uint64_t now;
    int ret;

    if (lsn == 0)
        return (EINVAL);

    memset(&kval, 0, sizeof(kval));
    memset(&vval, 0, sizeof(vval));
    memset(matches, 0, sizeof(*matches));
    memset(&page_key, 0, sizeof(page_key));
    memset(&result_key, 0, sizeof(result_key));
    readonly_result_key = NULL;
    now = palm_kv_timestamp_us();

    matches->context = context;
    matches->table_id = table_id;
    matches->page_id = page_id;
    matches->query_lsn = lsn;

    page_key.table_id = table_id;
    page_key.page_id = page_id;
    page_key.lsn = lsn;
    swap_page_key(&page_key, &page_key);
    kval.mv_size = sizeof(page_key);
    kval.mv_data = &page_key;
    if ((ret = mdb_cursor_open(
           context->lmdb_txn, context->env->lmdb_pages_dbi, &matches->lmdb_cursor)) != 0)
        return (ret);
    ret = mdb_cursor_get(matches->lmdb_cursor, &kval, &vval, MDB_SET_RANGE);
    if (ret == MDB_NOTFOUND) {
        /* If we went off the end, go to the last record. */
        ret = mdb_cursor_get(matches->lmdb_cursor, &kval, &vval, MDB_LAST);
    }
    if (ret == 0) {
        if (kval.mv_size != sizeof(PAGE_KEY))
            return (EIO); /* not expected, data damaged, could be assert */
        readonly_result_key = (PAGE_KEY *)kval.mv_data;
        swap_page_key(readonly_result_key, &result_key);
    }
    /*
     * Now back up until we get a match. This will be the last valid record that matches the
     * table/page.
     */
    while (ret == 0 && !RESULT_MATCH(&result_key, context, table_id, page_id, lsn, now)) {
        ret = mdb_cursor_get(matches->lmdb_cursor, &kval, &vval, MDB_PREV);
        readonly_result_key = (PAGE_KEY *)kval.mv_data;
        swap_page_key(readonly_result_key, &result_key);
    }
    /*
     * Now back up until we find the most recent full page that does not have a checkpoint more
     * recent than asked for.
     */
    while (ret == 0 && RESULT_MATCH(&result_key, context, table_id, page_id, lsn, now)) {
        /* If this is what we're looking for, we're done, and the cursor is positioned. */
        if (result_key.is_delta == false) {
            matches->lsn = result_key.lsn;
            matches->size = vval.mv_size;
            matches->data = vval.mv_data;
            matches->backlink_lsn = result_key.backlink_lsn;
            matches->base_lsn = result_key.base_lsn;
            matches->encryption = result_key.encryption;
            matches->first = true;
            return (0);
        }
        ret = mdb_cursor_get(matches->lmdb_cursor, &kval, &vval, MDB_PREV);
        readonly_result_key = (PAGE_KEY *)kval.mv_data;
        swap_page_key(readonly_result_key, &result_key);
    }
    if (ret == MDB_NOTFOUND) {
        /* We're done, there are no matches. */
        mdb_cursor_close(matches->lmdb_cursor);
        matches->lmdb_cursor = NULL;
        return (0);
    }
    matches->error = ret;
    return (ret);
}

bool
palm_kv_next_page_match(PALM_KV_PAGE_MATCHES *matches)
{
    MDB_val kval;
    MDB_val vval;
    PAGE_KEY *readonly_page_key;
    PAGE_KEY page_key;
    PALM_KV_CONTEXT *context;
    uint64_t now;
    int ret;

    context = matches->context;

    if (matches->lmdb_cursor == NULL)
        return (false);

    now = palm_kv_timestamp_us();

    memset(&page_key, 0, sizeof(page_key));
    memset(&kval, 0, sizeof(kval));
    memset(&vval, 0, sizeof(vval));
    if (matches->first) {
        /*
         * We already have the value set from the positioning. Return the value, and set up to
         * advance the next time.
         */
        matches->first = false;
        return (true);
    }

    ret = mdb_cursor_get(matches->lmdb_cursor, &kval, &vval, MDB_NEXT);
    if (ret == 0) {
        if (kval.mv_size != sizeof(PAGE_KEY))
            return (EIO); /* not expected, data damaged, could be assert */
        readonly_page_key = (PAGE_KEY *)kval.mv_data;
        swap_page_key(readonly_page_key, &page_key);

        if (RESULT_MATCH(
              &page_key, context, matches->table_id, matches->page_id, matches->query_lsn, now)) {
            matches->lsn = page_key.lsn;
            matches->size = vval.mv_size;
            matches->data = vval.mv_data;
            matches->backlink_lsn = page_key.backlink_lsn;
            matches->base_lsn = page_key.base_lsn;
            matches->encryption = page_key.encryption;
            matches->flags = page_key.flags;
            return (true);
        }
    }

    /* There are no more matches, or there was an error, so close the cursor. */
    mdb_cursor_close(matches->lmdb_cursor);
    matches->lmdb_cursor = NULL;
    if (ret != MDB_NOTFOUND)
        matches->error = ret;
    return (false);
}

int
palm_kv_put_checkpoint(PALM_KV_CONTEXT *context, uint64_t checkpoint_lsn,
  uint64_t checkpoint_timestamp, const WT_ITEM *checkpoint_metadata)
{
    CKPT_KEY ckpt_key;
    MDB_val kval;
    MDB_val vval;

    memset(&ckpt_key, 0, sizeof(ckpt_key));
    memset(&kval, 0, sizeof(kval));
    memset(&vval, 0, sizeof(kval));

    ckpt_key.lsn = checkpoint_lsn;
    ckpt_key.checkpoint_timestamp = checkpoint_timestamp;
    swap_ckpt_key(&ckpt_key, &ckpt_key);

    kval.mv_size = sizeof(ckpt_key);
    kval.mv_data = &ckpt_key;
    vval.mv_size = checkpoint_metadata == NULL ? 0 : checkpoint_metadata->size;
    vval.mv_data = checkpoint_metadata == NULL ? (void *)"" : (void *)checkpoint_metadata->data;
    return (mdb_put(context->lmdb_txn, context->env->lmdb_ckpt_dbi, &kval, &vval, 0));
}

int
palm_kv_get_last_checkpoint(PALM_KV_CONTEXT *context, uint64_t *checkpoint_lsn,
  uint64_t *checkpoint_timestamp, void **checkpoint_metadata, size_t *checkpoint_metadata_size)
{
    CKPT_KEY ckpt_key;
    MDB_cursor *cursor;
    MDB_val kval;
    MDB_val vval;
    int ret;

    cursor = NULL;
    memset(&ckpt_key, 0, sizeof(ckpt_key));
    memset(&kval, 0, sizeof(kval));
    memset(&vval, 0, sizeof(vval));

    if ((ret = mdb_cursor_open(context->lmdb_txn, context->env->lmdb_ckpt_dbi, &cursor)) != 0)
        return (ret);
    ret = mdb_cursor_get(cursor, &kval, &vval, MDB_LAST);
    mdb_cursor_close(cursor);
    if (ret != 0)
        return (ret);

    if (kval.mv_size != sizeof(CKPT_KEY))
        return (EINVAL);
    ckpt_key = *(CKPT_KEY *)kval.mv_data;
    swap_ckpt_key(&ckpt_key, &ckpt_key);

    if (checkpoint_lsn != NULL)
        *checkpoint_lsn = ckpt_key.lsn;
    if (checkpoint_timestamp != NULL)
        *checkpoint_timestamp = ckpt_key.checkpoint_timestamp;
    if (checkpoint_metadata != NULL)
        *checkpoint_metadata = vval.mv_data;
    if (checkpoint_metadata_size != NULL)
        *checkpoint_metadata_size = vval.mv_size;

    return (0);
}
