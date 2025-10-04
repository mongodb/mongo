/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __conn_page_history_print --
 *     Print information about a page.
 */
static int
__conn_page_history_print(WT_SESSION_IMPL *session, const WT_PAGE_HISTORY_ITEM *item)
{
    uint64_t total_read_count_between_reads, total_time_between_reads;
    const char *page_type_str;

    if (item == NULL) {
        WT_RET(__wt_msg(session, "  none"));
        return (0);
    }

    page_type_str = "other";
    switch (item->page_type) {
    case WT_PAGE_ROW_INT:
        page_type_str = "WT_PAGE_ROW_INT";
        break;
    case WT_PAGE_ROW_LEAF:
        page_type_str = "WT_PAGE_ROW_LEAF";
        break;
    }

    total_read_count_between_reads = item->last_global_read_count - item->first_global_read_count;
    total_time_between_reads = item->last_read_timestamp - item->first_read_timestamp;

    WT_RET(__wt_msg(session, "  table ID                         : %" PRIu32, item->key.table_id));
    WT_RET(__wt_msg(session, "  page ID                          : %" PRIu64, item->key.page_id));
    WT_RET(__wt_msg(session, "  page type                        : %" PRIu8 " (%s)",
      item->page_type, page_type_str));
    WT_RET(__wt_msg(session, "  number of reads                  : %" PRIu32, item->num_reads));
    WT_RET(__wt_msg(session, "  number of evictions              : %" PRIu32, item->num_evicts));
    WT_RET(__wt_msg(session, "  avg. time between re-reads (ms)  : %" PRIu64,
      item->num_reads <= 1 ? 0 : total_time_between_reads / (item->num_reads - 1)));
    WT_RET(__wt_msg(session, "  avg. other reads between re-reads: %" PRIu64,
      item->num_reads <= 1 ? 0 : total_read_count_between_reads / (item->num_reads - 1)));

    return (0);
}

/*
 * __conn_page_history_cmp_most_evicts --
 *     Compare function for sorting the most-evicted pages.
 */
static int WT_CDECL
__conn_page_history_cmp_most_evicts(const void *a, const void *b)
{
    WT_PAGE_HISTORY_ITEM *item_a, *item_b;
    item_a = (WT_PAGE_HISTORY_ITEM *)a;
    item_b = (WT_PAGE_HISTORY_ITEM *)b;

    if (item_a->num_evicts > item_b->num_evicts)
        return (-1);
    if (item_a->num_evicts < item_b->num_evicts)
        return (1);
    return (0);
}

/*
 * __conn_page_history_cmp_most_reads --
 *     Compare function for sorting the most-read pages.
 */
static int WT_CDECL
__conn_page_history_cmp_most_reads(const void *a, const void *b)
{
    WT_PAGE_HISTORY_ITEM *item_a, *item_b;
    item_a = (WT_PAGE_HISTORY_ITEM *)a;
    item_b = (WT_PAGE_HISTORY_ITEM *)b;

    if (item_a->num_reads > item_b->num_reads)
        return (-1);
    if (item_a->num_reads < item_b->num_reads)
        return (1);
    return (0);
}

/*
 * __conn_page_history_report --
 *     Report about the page history.
 */
static int
__conn_page_history_report(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    WT_HASH_MAP_ITEM *h;
    WT_PAGE_HISTORY *page_history;
    WT_PAGE_HISTORY_ITEM *item, *most_evicts, *most_reads;
    size_t i, top_n;
    bool empty, first;

    page_history = &S2C(session)->page_history;
    if (!page_history->enabled)
        return (0);

    most_evicts = NULL;
    most_reads = NULL;
    top_n = 5;

    WT_ERR(__wt_calloc_def(session, top_n, &most_evicts));
    WT_ERR(__wt_calloc_def(session, top_n, &most_reads));

    empty = true;
    for (i = 0; i < page_history->pages->hash_size; i++) {
        __wt_spin_lock(session, &page_history->pages->hash_locks[i]);
        TAILQ_FOREACH (h, &page_history->pages->hash[i], hashq) {
            item = (WT_PAGE_HISTORY_ITEM *)h->data;
            if (item->key.page_id == WT_BLOCK_INVALID_PAGE_ID)
                continue;
            if (item->num_evicts > most_evicts[top_n - 1].num_evicts) {
                most_evicts[top_n - 1] = *item;
                __wt_qsort(
                  most_evicts, top_n, sizeof(*most_evicts), __conn_page_history_cmp_most_evicts);
                empty = false;
            }
            if (item->num_reads > most_reads[top_n - 1].num_reads) {
                most_reads[top_n - 1] = *item;
                __wt_qsort(
                  most_reads, top_n, sizeof(*most_reads), __conn_page_history_cmp_most_reads);
                empty = false;
            }
        }
        __wt_spin_unlock(session, &page_history->pages->hash_locks[i]);
    }

    WT_ERR(__wt_msg(session, "%s", WT_DIVIDER));
    WT_ERR(__wt_msg(session, "page history report"));
    WT_ERR(__wt_msg(session, "  total reads    : %" PRIu64 " (%" PRIu64 " local)",
      page_history->global_read_count, page_history->global_read_count_local));
    WT_ERR(__wt_msg(session, "  total re-reads : %" PRIu64, page_history->global_reread_count));
    WT_ERR(__wt_msg(session,
      "  total evictions: %" PRIu64 " (%" PRIu64 " local, %" PRIu64 " without page ID)",
      page_history->global_evict_count, page_history->global_evict_count_local,
      page_history->global_evict_count_no_page_id));
    WT_ERR(__wt_msg(session, "%s", ""));

    if (empty) {
        WT_ERR(__wt_msg(session, "no pages read or evicted"));
        goto err;
    }

    WT_ERR(__wt_msg(session, "pages that were re-read the most:"));
    first = true;
    for (i = 0; i < top_n; i++) {
        if (most_reads[i].num_reads == 0)
            continue;
        if (first)
            first = false;
        else
            WT_ERR(__wt_msg(session, "%s", ""));
        WT_ERR(__conn_page_history_print(session, &most_reads[i]));
    }

    WT_ERR(__wt_msg(session, "%s", ""));
    WT_ERR(__wt_msg(session, "pages that were evicted the most:"));
    first = true;
    for (i = 0; i < top_n; i++) {
        if (most_evicts[i].num_evicts == 0)
            continue;
        if (first)
            first = false;
        else
            WT_ERR(__wt_msg(session, "%s", ""));
        WT_ERR(__conn_page_history_print(session, &most_evicts[i]));
    }

err:
    __wt_free(session, most_reads);
    __wt_free(session, most_evicts);
    return (ret);
}

/*
 * __conn_page_history_reporter --
 *     The thread for periodically reporting page history.
 */
static WT_THREAD_RET
__conn_page_history_reporter(void *arg)
{
    WT_DECL_RET;
    WT_PAGE_HISTORY *page_history;
    WT_SESSION_IMPL *session;
    uint32_t count;
    bool shutdown;

    session = arg;

    count = 0;
    page_history = &S2C(session)->page_history;

    for (;;) {
        /* Sleep for one second. Can be woken up early, e.g., for shutdown. */
        __wt_cond_wait(session, page_history->report_cond, WT_MILLION, NULL);

        WT_ACQUIRE_READ(shutdown, page_history->report_shutdown);
        if (shutdown)
            break;

        if (++count >= 30) {
            WT_ERR(__conn_page_history_report(session));
            count = 0;
        }
    }

    if (0) {
err:
        WT_IGNORE_RET(__wt_panic(session, ret, "page history reporter error"));
    }
    return (WT_THREAD_RET_VALUE);
}

/*
 * __wti_conn_page_history_config --
 *     Parse and setup page history for the connection.
 */
int
__wti_conn_page_history_config(WT_SESSION_IMPL *session, const char **cfg, bool reconfig)
{
    WT_CONFIG_ITEM cval;
    WT_PAGE_HISTORY *page_history;
    WT_SESSION_IMPL *report_session;
    bool enabled, previous_enabled;

    WT_UNUSED(reconfig);

    page_history = &S2C(session)->page_history;
    previous_enabled = page_history->enabled;

    WT_RET(__wt_config_gets(session, cfg, "debug_mode.page_history", &cval));
    enabled = cval.val != 0;

    /* Disabling page history reporting. */
    if (!enabled && previous_enabled) {

        /* Stop the reporting thread. */
        WT_RELEASE_WRITE(page_history->report_shutdown, (bool)true);
        __wt_cond_signal(session, page_history->report_cond);
        WT_RET(__wt_thread_join(session, &page_history->report_tid));
        page_history->report_tid_set = false;

        /*
         * Keep the rest of the state alive - at least for now, so that we don't have to handle
         * synchronization with all the other threads that might be in the middle of one of the
         * track calls below.
         */
    }

    /* Enabling page history reporting. */
    if (enabled && !previous_enabled) {

        /* Check if the state has been already initialized. */
        if (page_history->pages == NULL) {
            WT_RET(__wt_hash_map_init(session, &page_history->pages, 10 * WT_MILLION));
            page_history->pages->value_size = sizeof(WT_PAGE_HISTORY_ITEM);

            WT_RET(__wt_cond_alloc(session, "page-history-reporter", &page_history->report_cond));
            WT_RET(__wt_open_internal_session(
              S2C(session), "page-history-reporter", true, 0 /* flags */, 0, &report_session));
            page_history->report_session = report_session;
        }

        /* Start the reporting thread. */
        WT_RELEASE_WRITE(page_history->report_shutdown, (bool)false);
        WT_RET(__wt_thread_create(session, &page_history->report_tid, __conn_page_history_reporter,
          page_history->report_session));
        page_history->report_tid_set = true;
    }

    WT_RELEASE_WRITE(page_history->enabled, enabled);
    return (0);
}

/*
 * __wti_conn_page_history_destroy --
 *     Destroy page history.
 */
int
__wti_conn_page_history_destroy(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    WT_PAGE_HISTORY *page_history;

    page_history = &S2C(session)->page_history;

    if (page_history->enabled) {
        WT_TRET(__conn_page_history_report(session));
        page_history->enabled = false;
    }

    if (page_history->report_tid_set) {
        WT_RELEASE_WRITE(page_history->report_shutdown, (bool)true);
        __wt_cond_signal(session, page_history->report_cond);
        WT_TRET(__wt_thread_join(session, &page_history->report_tid));
        page_history->report_tid_set = false;
    }

    if (page_history->report_session != NULL) {
        WT_TRET(__wt_session_close_internal(page_history->report_session));
        page_history->report_session = NULL;
    }

    if (page_history->report_cond != NULL) {
        __wt_cond_destroy(session, &page_history->report_cond);
        page_history->report_cond = NULL;
    }

    __wt_hash_map_destroy(session, &page_history->pages);
    return (ret);
}

/*
 * __wt_conn_page_history_track_evict --
 *     Track page eviction.
 */
int
__wt_conn_page_history_track_evict(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_DECL_RET;
    WT_PAGE_HISTORY *page_history;
    WT_PAGE_HISTORY_ITEM *item;
    WT_PAGE_HISTORY_KEY key;
    uint64_t page_id;

    page_history = &S2C(session)->page_history;
    if (!page_history->enabled)
        return (0);

    WT_ACQUIRE_BARRIER();

    (void)__wt_atomic_add64(&page_history->global_evict_count, 1);

    /* So far this works only for disaggregated storage, as we don't have page IDs without it. */
    if (page->disagg_info == NULL) {
        (void)__wt_atomic_add64(&page_history->global_evict_count_local, 1);
        return (0);
    }

    page_id = page->disagg_info->block_meta.page_id;
    if (page_id == WT_BLOCK_INVALID_PAGE_ID) {
        (void)__wt_atomic_add64(&page_history->global_evict_count_no_page_id, 1);
        return (0);
    }

    item = NULL;
    memset(&key, 0, sizeof(key));
    key.page_id = page_id;
    key.table_id = S2BT(session)->id;

    WT_ERR(__wt_hash_map_get(
      session, page_history->pages, &key, sizeof(key), (void **)&item, NULL, true, true));

    item->key = key;
    item->page_type = page->type;

    item->num_evicts++;

err:
    if (item != NULL)
        __wt_hash_map_unlock(session, page_history->pages, &key, sizeof(key));
    return (ret);
}

/*
 * __wt_conn_page_history_track_read --
 *     Track page read.
 */
int
__wt_conn_page_history_track_read(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_DECL_RET;
    WT_PAGE_HISTORY *page_history;
    WT_PAGE_HISTORY_ITEM *item;
    WT_PAGE_HISTORY_KEY key;
    uint64_t current_ms, current_global_read_count, page_id;

    page_history = &S2C(session)->page_history;
    if (!page_history->enabled)
        return (0);

    WT_ACQUIRE_BARRIER();

    current_global_read_count = __wt_atomic_add64(&page_history->global_read_count, 1);

    /* So far this works only for disaggregated storage, as we don't have page IDs without it. */
    if (page->disagg_info == NULL) {
        (void)__wt_atomic_add64(&page_history->global_read_count_local, 1);
        return (0);
    }

    page_id = page->disagg_info->block_meta.page_id;
    if (page_id == WT_BLOCK_INVALID_PAGE_ID) /* This should not happen. */
        return (0);

    item = NULL;
    memset(&key, 0, sizeof(key));
    key.page_id = page_id;
    key.table_id = S2BT(session)->id;

    WT_ERR(__wt_hash_map_get(
      session, page_history->pages, &key, sizeof(key), (void **)&item, NULL, true, true));

    item->key = key;
    item->page_type = page->type;

    __wt_milliseconds(session, &current_ms);

    if (item->first_read_timestamp == 0)
        item->first_read_timestamp = current_ms;
    if (item->first_global_read_count == 0)
        item->first_global_read_count = current_global_read_count;

    item->last_global_read_count = current_global_read_count;
    item->last_read_timestamp = current_ms;
    item->num_reads++;

    if (item->num_reads > 1)
        (void)__wt_atomic_add64(&page_history->global_reread_count, 1);

err:
    if (item != NULL)
        __wt_hash_map_unlock(session, page_history->pages, &key, sizeof(key));
    return (ret);
}
