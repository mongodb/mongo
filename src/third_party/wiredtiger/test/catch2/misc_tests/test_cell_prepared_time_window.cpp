/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/* Unit tests for cell packing/unpacking of prepared time windows */
#include <catch2/catch.hpp>
#include <vector>
#include "../wrappers/mock_session.h"
#include "wt_internal.h"

/*
 * Helper function to create a test time window with specific prepared information
 */
static WT_TIME_WINDOW
create_test_time_window(wt_timestamp_t start_ts, uint64_t start_txn, wt_timestamp_t stop_ts,
  uint64_t stop_txn, bool has_start_prepare, bool has_stop_prepare,
  wt_timestamp_t start_prepare_ts = 100, uint64_t start_prepared_id = 1,
  wt_timestamp_t stop_prepare_ts = 200, uint64_t stop_prepared_id = 2)
{
    WT_TIME_WINDOW tw;
    WT_TIME_WINDOW_INIT(&tw);

    tw.start_ts = start_ts;
    tw.start_txn = start_txn;
    tw.stop_ts = stop_ts;
    tw.stop_txn = stop_txn;

    if (tw.start_ts != WT_TS_NONE)
        tw.durable_start_ts = tw.start_ts;

    if (tw.stop_ts != WT_TS_MAX)
        tw.durable_stop_ts = tw.stop_ts;

    if (has_start_prepare) {
        tw.start_prepare_ts = start_prepare_ts;
        tw.start_prepared_id = start_prepared_id;
    }

    if (has_stop_prepare) {
        tw.stop_prepare_ts = stop_prepare_ts;
        tw.stop_prepared_id = stop_prepared_id;
    }

    return tw;
}

/*
 * Helper function to pack a time window and return the packed data
 */
static std::vector<uint8_t>
pack_time_window(WT_SESSION_IMPL *session, WT_TIME_WINDOW *tw)
{
    /* Allocate enough space for the packed data (conservative estimate) */
    std::vector<uint8_t> buffer(256, 0);
    uint8_t *p = buffer.data();
    uint8_t *start_p = p;

    int ret = __cell_pack_value_validity(session, &p, tw);
    REQUIRE(ret == 0);

    /* Resize buffer to actual used size */
    size_t used_size = p - start_p;
    buffer.resize(used_size);

    return buffer;
}

/*
 * Helper function to unpack time window from packed data
 */
static WT_TIME_WINDOW
unpack_time_window(WT_SESSION_IMPL *session, const std::vector<uint8_t> &packed_data)
{
    /* Create a mock cell and page header for unpacking */
    WT_CELL cell;
    WT_PAGE_HEADER dsk;
    WT_CELL_UNPACK_KV unpack;

    memset(&cell, 0, sizeof(cell));
    memset(&dsk, 0, sizeof(dsk));
    memset(&unpack, 0, sizeof(unpack));

    /*
     * Initialize the page header with a reasonable write generation. This is needed because the
     * unpacking code checks dsk->write_gen.
     */
    dsk.write_gen = 2; /* Set to a value > base_write_gen to avoid cleanup */

    /*
     * The packed_data contains the validity window packed by __cell_pack_value_validity. We need to
     * create a proper WT_CELL_VALUE with this validity window.
     */

    uint8_t *p = cell.__chunk;
    int ret = 0;
    if (packed_data.size() <= 1) {
        /* Empty time window case */
        cell.__chunk[0] = WT_CELL_VALUE;
        p = &cell.__chunk[1];
        /* Pack zero data length */
        ret = __wt_vpack_uint(&p, 0, 0);
    } else {
        /* Has validity window */
        cell.__chunk[0] = WT_CELL_VALUE | WT_CELL_SECOND_DESC;

        /* Copy the validity window data (skip the first byte which was the descriptor increment) */
        memcpy(&cell.__chunk[1], packed_data.data() + 1, packed_data.size() - 1);

        /* Set pointer to after the validity window data */
        p = &cell.__chunk[packed_data.size()];

        /* Pack zero data length (WT_CELL_VALUE always expects a data length) */
        ret = __wt_vpack_uint(&p, 0, 0);
    }
    if (ret != 0) {
        throw std::runtime_error("Failed to pack zero data length");
    }

    /* Unpack the cell */
    __wt_cell_unpack_kv(session, &dsk, &cell, &unpack);

    return unpack.tw;
}

/*
 * Compare two time windows for equality
 */
static void
compare_time_windows(const WT_TIME_WINDOW &expected, const WT_TIME_WINDOW &actual)
{
    CHECK(expected.start_ts == actual.start_ts);
    CHECK(expected.start_txn == actual.start_txn);
    CHECK(expected.stop_ts == actual.stop_ts);
    CHECK(expected.stop_txn == actual.stop_txn);
    CHECK(expected.start_prepare_ts == actual.start_prepare_ts);
    CHECK(expected.start_prepared_id == actual.start_prepared_id);
    CHECK(expected.stop_prepare_ts == actual.stop_prepare_ts);
    CHECK(expected.stop_prepared_id == actual.stop_prepared_id);
}

TEST_CASE("Cell Time Window: Empty time window", "[cell][time_window]")
{
    std::shared_ptr<mock_session> session_mock = mock_session::build_test_mock_session();
    WT_CONNECTION_IMPL *conn;
    conn = S2C(session_mock->get_wt_session_impl());
    F_SET(conn, WT_CONN_PRESERVE_PREPARED);

    /* Set up the btree structure that the cell unpacking code needs */
    session_mock->setup_block_manager_file_operations();
    WT_BTREE *btree = (WT_BTREE *)session_mock->get_wt_session_impl()->dhandle->handle;
    btree->base_write_gen = 1; /* Initialize to a reasonable value */

    WT_TIME_WINDOW tw;
    WT_TIME_WINDOW_INIT(&tw);

    /* Empty time window should pack to minimal size */
    auto packed = pack_time_window(session_mock->get_wt_session_impl(), &tw);
    CHECK(packed.size() == 1); /* Just the increment for empty window */
}

TEST_CASE("Cell Time Window: Start prepared only", "[cell][time_window]")
{
    std::shared_ptr<mock_session> session_mock = mock_session::build_test_mock_session();
    WT_CONNECTION_IMPL *conn;
    conn = S2C(session_mock->get_wt_session_impl());
    F_SET(conn, WT_CONN_PRESERVE_PREPARED);

    /* Set up the btree structure that the cell unpacking code needs */
    session_mock->setup_block_manager_file_operations();
    WT_BTREE *btree = S2BT(session_mock->get_wt_session_impl());
    btree->base_write_gen = 1; /* Initialize to a reasonable value */

    SECTION("Basic start prepared")
    {
        auto tw = create_test_time_window(0, /* start_ts */
          10,                                /* start_txn */
          WT_TS_MAX,                         /* stop_ts */
          WT_TXN_MAX,                        /* stop_txn */
          true,                              /* has_start_prepare */
          false,                             /* has_stop_prepare */
          100,                               /* start_prepare_ts */
          1                                  /* start_prepared_id */
        );

        auto packed = pack_time_window(session_mock->get_wt_session_impl(), &tw);
        auto unpacked = unpack_time_window(session_mock->get_wt_session_impl(), packed);

        compare_time_windows(tw, unpacked);
    }
}

TEST_CASE("Cell Time Window: Stop prepared only", "[cell][time_window]")
{
    std::shared_ptr<mock_session> session_mock = mock_session::build_test_mock_session();
    WT_CONNECTION_IMPL *conn;
    conn = S2C(session_mock->get_wt_session_impl());
    F_SET(conn, WT_CONN_PRESERVE_PREPARED);

    /* Set up the btree structure that the cell unpacking code needs */
    session_mock->setup_block_manager_file_operations();
    WT_BTREE *btree = (WT_BTREE *)session_mock->get_wt_session_impl()->dhandle->handle;
    btree->base_write_gen = 1; /* Initialize to a reasonable value */

    SECTION("Basic stop prepared")
    {
        auto tw = create_test_time_window(50, /* start_ts */
          10,                                 /* start_txn */
          WT_TS_MAX,                          /* stop_ts */
          20,                                 /* stop_txn */
          false,                              /* has_start_prepare */
          true,                               /* has_stop_prepare */
          0,                                  /* start_prepare_ts (unused) */
          0,                                  /* start_prepared_id (unused) */
          200,                                /* stop_prepare_ts */
          3                                   /* stop_prepared_id */
        );

        auto packed = pack_time_window(session_mock->get_wt_session_impl(), &tw);
        auto unpacked = unpack_time_window(session_mock->get_wt_session_impl(), packed);

        compare_time_windows(tw, unpacked);
    }
}

TEST_CASE("Cell Time Window: Both start and stop prepared", "[cell][time_window]")
{
    std::shared_ptr<mock_session> session_mock = mock_session::build_test_mock_session();
    WT_CONNECTION_IMPL *conn;
    conn = S2C(session_mock->get_wt_session_impl());
    F_SET(conn, WT_CONN_PRESERVE_PREPARED);

    /* Set up the btree structure that the cell unpacking code needs */
    session_mock->setup_block_manager_file_operations();
    WT_BTREE *btree = (WT_BTREE *)session_mock->get_wt_session_impl()->dhandle->handle;
    btree->base_write_gen = 1; /* Initialize to a reasonable value */

    SECTION("Same transaction - both prepared")
    {
        auto tw = create_test_time_window(0, /* start_ts */
          10,                                /* start_txn */
          WT_TS_MAX,                         /* stop_ts */
          10,                                /* stop_txn (same as start) */
          true,                              /* has_start_prepare */
          true,                              /* has_stop_prepare */
          150,                               /* start_prepare_ts */
          2,                                 /* start_prepared_id */
          150,                               /* stop_prepare_ts (same as start) */
          2                                  /* stop_prepared_id (same as start) */
        );

        auto packed = pack_time_window(session_mock->get_wt_session_impl(), &tw);
        auto unpacked = unpack_time_window(session_mock->get_wt_session_impl(), packed);

        compare_time_windows(tw, unpacked);
    }
}

TEST_CASE("Cell Time Window: Regular (non-prepared) time window", "[cell][time_window]")
{
    std::shared_ptr<mock_session> session_mock = mock_session::build_test_mock_session();
    WT_CONNECTION_IMPL *conn;
    conn = S2C(session_mock->get_wt_session_impl());
    F_SET(conn, WT_CONN_PRESERVE_PREPARED);

    /* Set up the btree structure that the cell unpacking code needs */
    session_mock->setup_block_manager_file_operations();
    WT_BTREE *btree = (WT_BTREE *)session_mock->get_wt_session_impl()->dhandle->handle;
    btree->base_write_gen = 1; /* Initialize to a reasonable value */

    auto tw = create_test_time_window(50, /* start_ts */
      10,                                 /* start_txn */
      100,                                /* stop_ts */
      20,                                 /* stop_txn */
      false,                              /* has_start_prepare */
      false                               /* has_stop_prepare */
    );

    auto packed = pack_time_window(session_mock->get_wt_session_impl(), &tw);
    auto unpacked = unpack_time_window(session_mock->get_wt_session_impl(), packed);

    compare_time_windows(tw, unpacked);
}
