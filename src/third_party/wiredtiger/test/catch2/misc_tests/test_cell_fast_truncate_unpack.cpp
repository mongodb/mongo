/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/* Unit tests for reading fast-truncate page deletion info from addr-del cells. */
#include <catch2/catch.hpp>
#include <cstring>
#include "../wrappers/mock_session.h"
#include "wt_internal.h"

/*
 * build_ft_addr_del_cell --
 *     Build a fast-truncate addr-del cell for unpacking. When is_prepared is true, ts1 is the
 *     prepare timestamp and ts2 is the prepared transaction ID; when false, ts1 is the start commit
 *     timestamp and ts2 is the durable commit timestamp.
 */
static void
build_ft_addr_del_cell(WT_CELL *cell, bool is_prepared, uint64_t txnid, uint64_t ts1, uint64_t ts2)
{
    uint8_t *p = cell->__chunk;

    *p++ = WT_CELL_ADDR_DEL | WT_CELL_SECOND_DESC;
    *p++ = is_prepared ? WT_CELL_PREPARE : 0;

    WT_IGNORE_RET(__wt_vpack_uint(&p, 0, txnid));
    WT_IGNORE_RET(__wt_vpack_uint(&p, 0, ts1));
    WT_IGNORE_RET(__wt_vpack_uint(&p, 0, ts2));
    WT_IGNORE_RET(__wt_vpack_uint(&p, 0, 0)); /* zero-length address cookie */
}

/*
 * build_prepared_addr_cell --
 *     Build a regular addr cell marked as prepared. Used to verify that the prepare flag on a
 *     non-fast-truncate addr cell propagates to the time aggregate.
 */
static void
build_prepared_addr_cell(WT_CELL *cell)
{
    uint8_t *p = cell->__chunk;
    *p++ = WT_CELL_ADDR_LEAF | WT_CELL_SECOND_DESC;
    *p++ = WT_CELL_PREPARE;
    WT_IGNORE_RET(__wt_vpack_uint(&p, 0, 0)); /* zero-length address cookie */
}

static std::shared_ptr<mock_session>
setup_mock_session()
{
    std::shared_ptr<mock_session> session_mock = mock_session::build_test_mock_session();
    session_mock->setup_block_manager_file_operations();
    S2BT(session_mock->get_wt_session_impl())->base_write_gen = 1;
    return session_mock;
}

static WT_PAGE_HEADER
make_ft_page_header()
{
    WT_PAGE_HEADER dsk;
    memset(&dsk, 0, sizeof(dsk));
    dsk.write_gen = 2;
    F_SET(&dsk, WT_PAGE_FT_UPDATE);
    return dsk;
}

TEST_CASE("Cell Fast Truncate: committed deletion unpacks start and durable timestamps",
  "[cell][fast_truncate]")
{
    auto session_mock = setup_mock_session();
    WT_SESSION_IMPL *session = session_mock->get_wt_session_impl();

    WT_CELL cell;
    memset(&cell, 0, sizeof(cell));
    build_ft_addr_del_cell(&cell, /*is_prepared=*/false, /*txnid=*/10, /*start_ts=*/20,
      /*durable_ts=*/30);

    WT_PAGE_HEADER dsk = make_ft_page_header();
    WT_CELL_UNPACK_ADDR unpack;
    memset(&unpack, 0, sizeof(unpack));
    __wt_cell_unpack_addr(session, &dsk, &cell, &unpack);

    const WT_PAGE_DELETED &pd = unpack.page_del;
    CHECK(pd.txnid == 10);
    CHECK(pd.pg_del_start_ts == 20);
    CHECK(pd.pg_del_durable_ts == 30);
    /* A committed deletion is not in a prepare state. */
    CHECK(pd.prepare_state == WT_PREPARE_INIT);
    CHECK(pd.committed == true);
    CHECK(pd.selected_for_write == true);
    /* A committed fast-truncate does not mark the page as having prepared content. */
    CHECK(unpack.ta.prepare == 0);
}

TEST_CASE(
  "Cell Fast Truncate: prepared deletion unpacks prepare timestamp and ID, not commit timestamps",
  "[cell][fast_truncate]")
{
    auto session_mock = setup_mock_session();
    WT_SESSION_IMPL *session = session_mock->get_wt_session_impl();

    WT_CELL cell;
    memset(&cell, 0, sizeof(cell));
    build_ft_addr_del_cell(
      &cell, /*is_prepared=*/true, /*txnid=*/10, /*prepare_ts=*/15, /*prepared_id=*/5);

    WT_PAGE_HEADER dsk = make_ft_page_header();
    WT_CELL_UNPACK_ADDR unpack;
    memset(&unpack, 0, sizeof(unpack));
    __wt_cell_unpack_addr(session, &dsk, &cell, &unpack);

    const WT_PAGE_DELETED &pd = unpack.page_del;
    CHECK(pd.txnid == 10);
    CHECK(pd.prepare_ts == 15);
    /* Before commit, start_ts reflects the prepare timestamp. */
    CHECK(pd.pg_del_start_ts == 15);
    CHECK(pd.prepared_id == 5);
    CHECK(pd.prepare_state == WT_PREPARE_INPROGRESS);
    CHECK(pd.committed == false);
    CHECK(pd.selected_for_write == true);
    /* A prepared fast-truncate does not propagate prepare to the page-level visibility window. */
    CHECK(unpack.ta.prepare == 0);
}

TEST_CASE("Cell Fast Truncate Pack/Unpack: committed deletion round-trips correctly",
  "[cell][fast_truncate]")
{
    auto session_mock = setup_mock_session();
    WT_SESSION_IMPL *session = session_mock->get_wt_session_impl();

    WT_PAGE_HEADER dsk = make_ft_page_header();

    /* Build a committed page_del and pack it into a cell. */
    WT_PAGE_DELETED page_del;
    memset(&page_del, 0, sizeof(page_del));
    page_del.txnid = 42;
    page_del.pg_del_start_ts = 100;
    page_del.pg_del_durable_ts = 110;
    page_del.prepare_state = WT_PREPARE_INIT;
    page_del.committed = true;

    WT_TIME_AGGREGATE ta;
    WT_TIME_AGGREGATE_INIT(&ta);

    WT_CELL cell;
    memset(&cell, 0, sizeof(cell));
    WT_IGNORE_RET(__wt_cell_pack_addr(
      session, &cell, WT_CELL_ADDR_DEL, WT_RECNO_OOB, &page_del, &ta, false, 0));

    WT_CELL_UNPACK_ADDR unpack;
    memset(&unpack, 0, sizeof(unpack));
    __wt_cell_unpack_addr(session, &dsk, &cell, &unpack);

    const WT_PAGE_DELETED &pd = unpack.page_del;
    CHECK(pd.txnid == 42);
    CHECK(pd.pg_del_start_ts == 100);
    CHECK(pd.pg_del_durable_ts == 110);
    CHECK(pd.prepare_state == WT_PREPARE_INIT);
    CHECK(pd.committed == true);
    CHECK(pd.selected_for_write == true);
    CHECK(unpack.ta.prepare == 0);
}

TEST_CASE("Cell Fast Truncate Pack/Unpack: prepared deletion round-trips correctly",
  "[cell][fast_truncate]")
{
    auto session_mock = setup_mock_session();
    WT_SESSION_IMPL *session = session_mock->get_wt_session_impl();

    /* Both flags are required to pack a prepared fast-truncate cell. */
    F_SET(S2C(session), WT_CONN_PRESERVE_PREPARED);
    F_SET(S2BT(session), WT_BTREE_DISAGGREGATED);

    WT_PAGE_HEADER dsk = make_ft_page_header();

    /* Build a prepared page_del and pack it into a cell. */
    WT_PAGE_DELETED page_del;
    memset(&page_del, 0, sizeof(page_del));
    page_del.txnid = 99;
    page_del.prepare_ts = 50;
    page_del.prepared_id = 7;
    page_del.pg_del_durable_ts = WT_TS_NONE;
    page_del.prepare_state = WT_PREPARE_INPROGRESS;
    page_del.committed = false;

    WT_TIME_AGGREGATE ta;
    WT_TIME_AGGREGATE_INIT(&ta);

    WT_CELL cell;
    memset(&cell, 0, sizeof(cell));
    WT_IGNORE_RET(
      __wt_cell_pack_addr(session, &cell, WT_CELL_ADDR_DEL, WT_RECNO_OOB, &page_del, &ta, true, 0));

    WT_CELL_UNPACK_ADDR unpack;
    memset(&unpack, 0, sizeof(unpack));
    __wt_cell_unpack_addr(session, &dsk, &cell, &unpack);

    const WT_PAGE_DELETED &pd = unpack.page_del;
    CHECK(pd.txnid == 99);
    CHECK(pd.prepare_ts == 50);
    CHECK(pd.pg_del_start_ts == 50);
    CHECK(pd.prepared_id == 7);
    CHECK(pd.pg_del_durable_ts == WT_TS_NONE);
    CHECK(pd.prepare_state == WT_PREPARE_INPROGRESS);
    CHECK(pd.committed == false);
    CHECK(pd.selected_for_write == true);
    /* A prepared fast-truncate does not propagate prepare to the page-level visibility window. */
    CHECK(unpack.ta.prepare == 0);

    F_CLR(S2C(session), WT_CONN_PRESERVE_PREPARED);
    F_CLR(S2BT(session), WT_BTREE_DISAGGREGATED);
}

TEST_CASE("Cell Fast Truncate: prepare flag on non-fast-truncate addr cell marks page as prepared",
  "[cell][fast_truncate]")
{
    auto session_mock = setup_mock_session();
    WT_SESSION_IMPL *session = session_mock->get_wt_session_impl();

    WT_CELL cell;
    memset(&cell, 0, sizeof(cell));
    build_prepared_addr_cell(&cell);

    WT_PAGE_HEADER dsk;
    memset(&dsk, 0, sizeof(dsk));
    dsk.write_gen = 2;
    /* Not a fast-truncate page, so no deletion info is expected. */

    WT_CELL_UNPACK_ADDR unpack;
    memset(&unpack, 0, sizeof(unpack));
    __wt_cell_unpack_addr(session, &dsk, &cell, &unpack);

    /* The page-level visibility window must be marked as containing a prepared transaction. */
    CHECK(unpack.ta.prepare == 1);
    CHECK(unpack.page_del.txnid == 0);
    CHECK(unpack.page_del.selected_for_write == false);
}
