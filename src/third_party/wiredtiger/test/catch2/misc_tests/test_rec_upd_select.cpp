/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/* Unit tests for __rec_upd_select function */
#include <catch2/catch.hpp>
#include <vector>
#include "../wrappers/mock_session.h"
extern "C" {
#include "wt_internal.h"
#include "../../../src/reconcile/reconcile_private.h"
#include "../../../src/reconcile/reconcile_inline.h"
}
/*
 * Helper function to create a test update with specific properties
 */
static WT_UPDATE *
create_test_update(WT_SESSION_IMPL *session, const char *data, uint8_t type, uint64_t txnid,
  wt_timestamp_t start_ts, wt_timestamp_t durable_ts, uint8_t prepare_state = 0)
{
    WT_UPDATE *upd;
    size_t size;
    WT_ITEM value;

    if (data != NULL) {
        value.data = data;
        value.size = strlen(data);
    } else {
        value.data = NULL;
        value.size = 0;
    }

    /* Allocate update structure */
    if (__wt_upd_alloc(session, &value, type, &upd, &size) != 0)
        return NULL;

    /* Set transaction ID and timestamps */
    upd->txnid = txnid;
    upd->prepare_state = prepare_state;

    if (prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED) {
        upd->upd_start_ts = start_ts;
        upd->prepare_ts = start_ts;
    } else if (txnid == WT_TXN_ABORTED)
        /* TODO: save saved_id for aborted update but it's not needed for the current test cases */
        upd->upd_rollback_ts = start_ts;
    else {
        upd->upd_start_ts = start_ts;
        upd->upd_durable_ts = durable_ts;
    }
    return upd;
}

/*
 * Helper function to create a chain of updates (newest first)
 */
static WT_UPDATE *
create_update_chain(WT_SESSION_IMPL *session,
  std::vector<std::tuple<const char *, uint8_t, uint64_t, wt_timestamp_t, wt_timestamp_t, uint8_t>>
    &updates)
{
    WT_UPDATE *head = NULL, *prev = NULL;

    for (auto &upd_info : updates) {
        const char *data = std::get<0>(upd_info);
        uint8_t type = std::get<1>(upd_info);
        uint64_t txnid = std::get<2>(upd_info);
        wt_timestamp_t start_ts = std::get<3>(upd_info);
        wt_timestamp_t durable_ts = std::get<4>(upd_info);
        uint8_t prepare_state = std::get<5>(upd_info);

        WT_UPDATE *upd =
          create_test_update(session, data, type, txnid, start_ts, durable_ts, prepare_state);
        if (upd == NULL)
            return NULL;

        if (head == NULL)
            head = upd;
        else
            prev->next = upd;

        prev = upd;
    }

    return head;
}

static void
check_update(WT_UPDATE *upd,
  std::tuple<const char *, uint8_t, uint64_t, wt_timestamp_t, wt_timestamp_t, uint8_t>
    *expected_upd)
{
    if (!expected_upd) {
        REQUIRE(upd == NULL);
        return;
    }
    REQUIRE(upd != NULL);

    uint8_t type = std::get<1>(*expected_upd);
    uint64_t txnid = std::get<2>(*expected_upd);
    wt_timestamp_t start_ts = std::get<3>(*expected_upd);
    wt_timestamp_t durable_ts = std::get<4>(*expected_upd);
    uint8_t prepare_state = std::get<5>(*expected_upd);
    REQUIRE(upd->type == type);
    REQUIRE(upd->txnid == txnid);
    if (prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED) {
        REQUIRE(upd->upd_start_ts == start_ts);
        REQUIRE(upd->upd_durable_ts == 0);
    } else if (txnid == WT_TXN_ABORTED)
        /* TODO: save saved_id for aborted update but it's not needed for the current test cases */
        REQUIRE(upd->upd_rollback_ts == start_ts);
    else {
        REQUIRE(upd->upd_start_ts == start_ts);
        REQUIRE(upd->upd_durable_ts == durable_ts);
    }
}

/*
 * Helper function to setup minimal reconciliation context
 */
static void
setup_reconcile_context(
  WTI_RECONCILE *r, WT_PAGE *page, uint64_t pinned_id, wt_timestamp_t pinned_ts)
{
    memset(r, 0, sizeof(WTI_RECONCILE));
    r->page = page;
    r->rec_start_pinned_id = pinned_id;
    r->rec_start_pinned_stable_ts = pinned_ts;
    r->rec_start_oldest_id = pinned_id;
    r->rec_start_pinned_ts = pinned_ts;
    r->max_txn = WT_TXN_NONE;
    r->max_ts = WT_TS_NONE;
}

/*
 * Helper function to free update chain and insert
 */
static void
cleanup_test_data(WT_SESSION_IMPL *session, WT_INSERT *ins)
{
    WT_UPDATE *upd, *next;

    if (ins != NULL) {
        /* Free update chain */
        for (upd = ins->upd; upd != NULL; upd = next) {
            next = upd->next;
            __wt_free(session, upd);
        }
        /* Free insert */
        __wt_free(session, ins);
    }
}

/*
 * Helper function to create a test WT_INSERT structure
 */
static WT_INSERT *
create_test_insert(WT_SESSION_IMPL *session, WT_UPDATE *upd_chain)
{
    WT_INSERT *ins;
    const char *key_data = "key1";
    size_t key_size = 4;
    /* Simple skiplist depth */
    u_int skipdepth = 1;
    size_t ins_size;

    /*
     * Allocate the WT_INSERT structure, next pointers for the skip list, and room for the key. This
     * follows the same pattern as __row_insert_alloc in src/btree/row_modify.c
     */
    ins_size = sizeof(WT_INSERT) + skipdepth * sizeof(WT_INSERT *) + key_size;
    if (__wt_calloc(session, 1, ins_size, &ins) != 0)
        return NULL;

    ins->upd = upd_chain;
    ins->u.key.offset = WT_STORE_SIZE(ins_size - key_size);
    ins->u.key.size = WT_STORE_SIZE(key_size);

    /* Copy the key into place */
    memcpy(WT_INSERT_KEY(ins), key_data, key_size);

    return ins;
}

struct RecUpdSelectFixture {
    RecUpdSelectFixture() : mock(mock_session::build_test_mock_session())
    {
        mock->setup_block_manager_file_operations();
        session = mock->get_wt_session_impl();
        session->id = 0;
        WT_TXN_SHARED *txn_shared_list;
        REQUIRE(__wt_calloc(session, 1, sizeof(WT_TXN_SHARED), &txn_shared_list) == 0);
        S2C(session)->txn_global.txn_shared_list = txn_shared_list;
        /* Allocate and set up transaction structure */
        REQUIRE(__wt_calloc(session, 1, sizeof(WT_TXN), &session->txn) == 0);
        REQUIRE(__wt_calloc(session, 1, sizeof(WT_PAGE), &page) == 0);
        page->type = WT_PAGE_ROW_LEAF;
    }

    ~RecUpdSelectFixture()
    {
        __wt_free(session, page);
        __wt_free(session, session->txn);
        __wt_free(session, S2C(session)->txn_global.txn_shared_list);
    }

    std::shared_ptr<mock_session> mock;
    WT_SESSION_IMPL *session;
    WT_PAGE *page;
};

TEST_CASE_METHOD(RecUpdSelectFixture, "rec_upd_select: Basic visible update selection",
  "[reconcile][rec_upd_select]")
{
    /* Set up transaction with snapshot for visibility checks */
    F_SET(session->txn, WT_TXN_HAS_SNAPSHOT);
    session->txn->snapshot_data.snap_max = 200;
    session->txn->id = 120;
    session->txn->isolation = WT_ISO_SNAPSHOT;
    F_CLR(session->dhandle, WT_DHANDLE_HS);

    /* Setup reconciliation context with pinned transaction ID 120 */
    WTI_RECONCILE r;
    setup_reconcile_context(&r, page, 120, 50);
    std::vector<
      /* data, update type, txn id, timestamp, durable ts, prepare_ts */
      std::tuple<const char *, uint8_t, uint64_t, wt_timestamp_t, wt_timestamp_t, uint8_t>>
      updates = {std::make_tuple("value3", (uint8_t)WT_UPDATE_STANDARD, (uint64_t)120,
                   (wt_timestamp_t)30, (wt_timestamp_t)30, (uint8_t)0),
        std::make_tuple("value2", (uint8_t)WT_UPDATE_STANDARD, (uint64_t)80, (wt_timestamp_t)20,
          (wt_timestamp_t)20, (uint8_t)0),
        std::make_tuple("value1", (uint8_t)WT_UPDATE_STANDARD, (uint64_t)50, (wt_timestamp_t)10,
          (wt_timestamp_t)10, (uint8_t)0)};

    WT_UPDATE *update_chain = create_update_chain(session, updates);
    REQUIRE(update_chain != NULL);

    WT_INSERT *ins = create_test_insert(session, update_chain);
    REQUIRE(ins != NULL);

    SECTION("In memory, should write oldest update in the list")
    {
        F_SET(S2C(session), WT_CONN_IN_MEMORY);
        WTI_UPDATE_SELECT upd_select;
        WTI_UPDATE_SELECT_INIT(&upd_select);

        int ret = __wti_rec_upd_select(session, &r, ins, NULL, NULL, &upd_select);

        REQUIRE(ret == 0);
        check_update(upd_select.upd, &updates[2]);

        // Should track the newest transaction in max_txn
        REQUIRE(r.max_txn == 120);
        REQUIRE(r.max_ts == 30);
    }
    SECTION("Not in-memory, should write newest update in the list")
    {
        F_CLR(S2C(session), WT_CONN_IN_MEMORY);
        WTI_UPDATE_SELECT upd_select;
        WTI_UPDATE_SELECT_INIT(&upd_select);

        /* Call the function under test */
        int ret = __wti_rec_upd_select(session, &r, ins, NULL, NULL, &upd_select);

        // Verify results
        REQUIRE(ret == 0);
        check_update(upd_select.upd, &updates[0]);

        /* Should track the newest transaction in max_txn */
        REQUIRE(r.max_txn == 120);
        REQUIRE(r.max_ts == 30);
    }
    cleanup_test_data(session, ins);
}

TEST_CASE_METHOD(
  RecUpdSelectFixture, "rec_upd_select: select non-pruned update", "[reconcile][rec_upd_select]")
{
    /* Set up transaction with snapshot for visibility checks */
    F_SET(session->txn, WT_TXN_HAS_SNAPSHOT);
    session->txn->snapshot_data.snap_max = 200;
    session->txn->id = 120;
    session->txn->isolation = WT_ISO_SNAPSHOT;

    WTI_RECONCILE r;
    setup_reconcile_context(&r, page, 120, 50);
    r.rec_prune_timestamp = 10;
    std::vector<
      /* data, update type, txn id, start timestamp, durable ts, prepare_state */
      std::tuple<const char *, uint8_t, uint64_t, wt_timestamp_t, wt_timestamp_t, uint8_t>>
      updates = {std::make_tuple("value3", (uint8_t)WT_UPDATE_STANDARD, (uint64_t)120,
                   (wt_timestamp_t)30, (wt_timestamp_t)30, (uint8_t)0),
        /* update with durable timestamp > pruned timestamp */
        std::make_tuple("value2", (uint8_t)WT_UPDATE_STANDARD, (uint64_t)80, (wt_timestamp_t)20,
          (wt_timestamp_t)20, (uint8_t)0),
        /* update with durable timestamp == pruned timestamp, should not be selected */
        std::make_tuple("value1", (uint8_t)WT_UPDATE_STANDARD, (uint64_t)50, (wt_timestamp_t)10,
          (wt_timestamp_t)10, (uint8_t)0)};

    WT_UPDATE *update_chain = create_update_chain(session, updates);
    REQUIRE(update_chain != NULL);

    WT_INSERT *ins = create_test_insert(session, update_chain);
    REQUIRE(ins != NULL);

    SECTION("In-memory, should write oldest update with timestamp > prune timestamp in the list")
    {
        F_SET(S2C(session), WT_CONN_IN_MEMORY);
        WTI_UPDATE_SELECT upd_select;
        WTI_UPDATE_SELECT_INIT(&upd_select);

        int ret = __wti_rec_upd_select(session, &r, ins, NULL, NULL, &upd_select);

        REQUIRE(ret == 0);
        /* Should select updates[1] because updates[2] is pruned */
        check_update(upd_select.upd, &updates[1]);

        /* Should track the newest transaction in max_txn */
        REQUIRE(r.max_txn == 120);
        REQUIRE(r.max_ts == 30);
    }
    cleanup_test_data(session, ins);
}

TEST_CASE_METHOD(RecUpdSelectFixture, "rec_upd_select: Skip writing aborted and prepared updates",
  "[reconcile][rec_upd_select]")
{
    /* Set up transaction with snapshot for visibility checks */
    F_SET(session->txn, WT_TXN_HAS_SNAPSHOT);
    session->txn->snapshot_data.snap_max = 200;
    session->txn->id = 120;
    session->txn->isolation = WT_ISO_SNAPSHOT;

    WTI_RECONCILE r;
    setup_reconcile_context(&r, page, 120, 50);
    r.rec_prune_timestamp = 10;
    std::vector<
      /* data, update type, txn id, start timestamp, durable ts, prepare_state */
      std::tuple<const char *, uint8_t, uint64_t, wt_timestamp_t, wt_timestamp_t, uint8_t>>
      updates = {/* prepared update */
        std::make_tuple("value4", (uint8_t)WT_UPDATE_STANDARD, (uint64_t)120, (wt_timestamp_t)40,
          (wt_timestamp_t)30, (uint8_t)WT_PREPARE_INPROGRESS),
        /* aborted update */
        std::make_tuple("value3", (uint8_t)WT_UPDATE_STANDARD, (uint64_t)WT_TXN_ABORTED,
          (wt_timestamp_t)20, (wt_timestamp_t)30, (uint8_t)0),
        /* standard update */
        std::make_tuple("value2", (uint8_t)WT_UPDATE_STANDARD, (uint64_t)50, (wt_timestamp_t)10,
          (wt_timestamp_t)20, (uint8_t)0)};

    WT_UPDATE *update_chain = create_update_chain(session, updates);
    REQUIRE(update_chain != NULL);

    WT_INSERT *ins = create_test_insert(session, update_chain);
    REQUIRE(ins != NULL);

    SECTION("In-memory, should write oldest update with timestamp > prune timestamp in the list")
    {
        F_SET(S2C(session), WT_CONN_IN_MEMORY);
        WTI_UPDATE_SELECT upd_select;
        WTI_UPDATE_SELECT_INIT(&upd_select);

        int ret = __wti_rec_upd_select(session, &r, ins, NULL, NULL, &upd_select);

        REQUIRE(ret == 0);
        /* Should skip all prepared and aborted update, select the standard update */
        check_update(upd_select.upd, &updates[2]);

        REQUIRE(r.max_txn == 120);
        REQUIRE(r.max_ts == 40);
    }

    SECTION("Not In-memory, should write newest update")
    {
        F_CLR(S2C(session), WT_CONN_IN_MEMORY);
        F_SET(&r, WT_REC_EVICT);
        WTI_UPDATE_SELECT upd_select;
        WTI_UPDATE_SELECT_INIT(&upd_select);

        int ret = __wti_rec_upd_select(session, &r, ins, NULL, NULL, &upd_select);

        REQUIRE(ret == 0);
        /* Should write the prepared update */
        check_update(upd_select.upd, &updates[0]);

        /* Should track the newest transaction in max_txn */
        REQUIRE(r.max_txn == 120);
        REQUIRE(r.max_ts == 40);
    }
    cleanup_test_data(session, ins);
    /*  clean up saved update */
    if (r.supd)
        __wt_free(session, r.supd);
}
