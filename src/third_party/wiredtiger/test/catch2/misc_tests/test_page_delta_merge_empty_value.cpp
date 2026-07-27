/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Unit test for __wti_page_merge_deltas_with_base_image_leaf: rebuilding a leaf page from its base
 * image and deltas must not drop a live key stored as an empty-value cell.
 *
 * An empty-value cell (a key with no value cell -- a zero-length value with an empty time window)
 * has no stop of its own; for the last entry on a page the unpacked value still holds the previous
 * entry's time window. The logic that skips globally visible deletes must not consult that stale
 * window for an empty-value cell, or it drops the live key.
 */
#include <catch2/catch.hpp>
#include <cstring>
#include "../wrappers/mock_session.h"

extern "C" {
#include "wt_internal.h"
}

namespace {

static void
init_disk_state(WT_SESSION_IMPL *session, WT_ITEM *img, WTI_DISK_LEAF_MERGE_STATE *s)
{
    WT_BTREE *btree = S2BT(session);
    memset(s, 0, sizeof(*s));
    /*
     * __wt_cell_pack_leaf_kv appends at img->mem + img->size, so reserve the page header up front
     * (matching how the real merge seeds new_image->size before packing cells).
     */
    img->size = WT_PAGE_HEADER_BYTE_SIZE(btree);
    s->cell_ptr = (uint8_t *)WT_PAGE_HEADER_BYTE(btree, img->mem);
    s->all_empty_value = true;
    s->any_empty_value = false;
    s->entries = 0;
    s->key_pfx_last = 0;
    s->key_pfx_compress = false; /* No prefix compression: keys are stored in full. */
    REQUIRE(__wt_scr_alloc(session, 0, &s->last_key) == 0);
}

static void
finalize_leaf_image(WT_ITEM *img, WTI_DISK_LEAF_MERGE_STATE *s)
{
    WT_PAGE_HEADER *dsk = (WT_PAGE_HEADER *)img->mem;
    memset(dsk, 0, WT_PAGE_HEADER_SIZE);
    dsk->u.entries = s->entries;
    dsk->type = WT_PAGE_ROW_LEAF;
    dsk->version = WT_PAGE_VERSION_TS;
    dsk->write_gen = 1000; /* High: keep read-side transaction-id cleanup out of the picture. */
    img->size = WT_PTRDIFF(s->cell_ptr, img->mem);
    dsk->mem_size = WT_STORE_SIZE(img->size);
}

static bool
image_has_key(WT_ITEM *img, const char *key, size_t key_size)
{
    const uint8_t *p = (const uint8_t *)img->mem;
    if (key_size == 0 || img->size < key_size)
        return false;
    for (size_t i = 0; i + key_size <= img->size; i++)
        if (memcmp(p + i, key, key_size) == 0)
            return true;
    return false;
}

struct merge_empty_value_fixture {
    std::shared_ptr<mock_session> mock;
    WT_SESSION_IMPL *session;

    merge_empty_value_fixture() : mock(mock_session::build_test_mock_session())
    {
        mock->setup_block_manager_file_operations();
        session = mock->get_wt_session_impl();
        session->id = 0;

        WT_TXN_SHARED *txn_shared_list;
        REQUIRE(__wt_calloc(session, 1, sizeof(WT_TXN_SHARED), &txn_shared_list) == 0);
        S2C(session)->txn_global.txn_shared_list = txn_shared_list;
        REQUIRE(__wt_calloc(session, 1, sizeof(WT_TXN), &session->txn) == 0);

        WT_TXN_GLOBAL *txn_global = &S2C(session)->txn_global;
        txn_global->oldest_id = 100;
        txn_global->pinned_timestamp = 100;
        txn_global->oldest_timestamp = 100;
        txn_global->has_pinned_timestamp = true;
        txn_global->checkpoint_txn_shared.pinned_id = WT_TXN_NONE;

        WT_BTREE *btree = S2BT(session);
        btree->collator = nullptr;
        btree->prefix_compression = false;
        btree->block_header = WT_BLOCK_HEADER_SIZE;
        btree->base_write_gen = 1;
    }

    ~merge_empty_value_fixture()
    {
        __wt_free(session, session->txn);
        __wt_free(session, S2C(session)->txn_global.txn_shared_list);
    }
};

} // namespace

TEST_CASE_METHOD(merge_empty_value_fixture,
  "merge deltas keeps an empty-value last key with a globally visible neighbor",
  "[page_delta_merge]")
{
    WT_ITEM base, delta, new_image;
    WT_CLEAR(base);
    WT_CLEAR(delta);
    WT_CLEAR(new_image);
    REQUIRE(__wt_buf_init(session, &base, 4096) == 0);
    REQUIRE(__wt_buf_init(session, &delta, 4096) == 0);
    REQUIRE(__wt_buf_init(session, &new_image, 8192) == 0);

    /*
     * Base image: "aaa" is a value cell with a globally visible stop (a delete that should be
     * dropped); "zzz" is an empty-value cell (zero-length value, empty window) and the last entry.
     */
    WTI_DISK_LEAF_MERGE_STATE bs;
    init_disk_state(session, &base, &bs);
    WT_TIME_WINDOW tw_stop;
    WT_TIME_WINDOW_INIT(&tw_stop);
    tw_stop.stop_ts = tw_stop.durable_stop_ts = 5; /* 5 <= pinned 100: globally visible */
    tw_stop.stop_txn = WT_TXN_NONE;
    REQUIRE(
      __wt_cell_pack_leaf_kv(session, false, "aaa", 3, "value1", 6, &tw_stop, &base, &bs) == 0);
    WT_TIME_WINDOW tw_empty;
    WT_TIME_WINDOW_INIT(&tw_empty);
    REQUIRE(
      __wt_cell_pack_leaf_kv(session, true, "zzz", 3, nullptr, 0, &tw_empty, &base, &bs) == 0);
    finalize_leaf_image(&base, &bs);
    REQUIRE(((WT_PAGE_HEADER *)base.mem)->u.entries == 3); /* aaa key+value, zzz key */
    __wt_scr_free(session, &bs.last_key);

    /*
     * An empty delta (header only, no entries) is enough to invoke the merge; it then walks the
     * base image, where "zzz" is the last entry, exercising the empty-value/last-entry path.
     */
    WTI_DISK_LEAF_MERGE_STATE ds;
    init_disk_state(session, &delta, &ds);
    finalize_leaf_image(&delta, &ds);
    __wt_scr_free(session, &ds.last_key);

    int ret;
#ifdef HAVE_DIAGNOSTIC
    WT_TIME_AGGREGATE ta;
    ret = __wti_page_merge_deltas_with_base_image_leaf(
      session, &delta, 1, &new_image, (WT_PAGE_HEADER *)base.mem, &ta);
#else
    ret = __wti_page_merge_deltas_with_base_image_leaf(
      session, &delta, 1, &new_image, (WT_PAGE_HEADER *)base.mem);
#endif
    REQUIRE(ret == 0);

    /*
     * Exactly one entry must survive: "aaa" (globally visible stop) is dropped, and the empty-value
     * last key "zzz" is kept. Because "aaa" is gone and one entry remains, that entry is "zzz". The
     * buggy version consulted the empty-value cell's stale time window (the adjacent "aaa" stop)
     * and dropped it too, leaving zero entries.
     */
    REQUIRE(((WT_PAGE_HEADER *)new_image.mem)->u.entries == 1);
    REQUIRE(image_has_key(&new_image, "zzz", 3));  /* live empty-value last key survived */
    REQUIRE(!image_has_key(&new_image, "aaa", 3)); /* globally visible stop was dropped */

    __wt_buf_free(session, &base);
    __wt_buf_free(session, &delta);
    __wt_buf_free(session, &new_image);
}
