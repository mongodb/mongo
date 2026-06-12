/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wiredtiger.h"
#include "wt_internal.h"
#include "../wrappers/connection_wrapper.h"
#include "../utils.h"

extern "C" {
#include "../../../src/reconcile/reconcile_private.h"
#include "../../../src/reconcile/reconcile_inline.h"
}

/*
 * A block manager free implementation that always fails. Used to exercise the error path in
 * __ovfl_reuse_wrapup.
 */
static int
block_free_fail(
  WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size, bool checkpoint_io)
{
    (void)bm;
    (void)session;
    (void)addr;
    (void)addr_size;
    (void)checkpoint_io;
    return (EINVAL);
}

static void
setup_failing_bm(WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle, WT_BTREE *btree, WT_BM *bm)
{
    memset(bm, 0, sizeof(*bm));
    bm->free = block_free_fail;

    memset(btree, 0, sizeof(*btree));
    btree->bm = bm;

    memset(dhandle, 0, sizeof(*dhandle));
    dhandle->handle = btree;

    session->dhandle = dhandle;
}

TEST_CASE("Reconciliation tracking: ovfl_track_init", "[reconciliation]")
{
    connection_wrapper conn(DB_HOME);
    WT_SESSION_IMPL *session = conn.create_session();

    WT_PAGE p;
    memset(&p, 0, sizeof(p));

    WT_PAGE_MODIFY m;
    memset(&m, 0, sizeof(m));

    p.modify = &m;

    REQUIRE(__ut_ovfl_track_init(session, &p) == 0);
    REQUIRE(m.ovfl_track != nullptr);
    __wt_free(session, m.ovfl_track);
}

TEST_CASE("Reconciliation tracking: ovfl_discard_verbose", "[reconciliation]")
{
    connection_wrapper conn(DB_HOME);
    WT_SESSION_IMPL *session = conn.create_session();

    SECTION("handle null page and tag")
    {
        REQUIRE(__ut_ovfl_discard_verbose(session, nullptr, nullptr, nullptr) == EINVAL);
    }
}

TEST_CASE("Reconciliation tracking: ovfl_discard_wrapup", "[reconciliation]")
{
    connection_wrapper conn(DB_HOME);
    WT_SESSION_IMPL *session = conn.create_session();

    WT_PAGE p;
    memset(&p, 0, sizeof(p));

    WT_PAGE_MODIFY m;
    memset(&m, 0, sizeof(m));
    p.modify = &m;

    REQUIRE(__ut_ovfl_track_init(session, &p) == 0);

    SECTION("handle empty overflow entry list")
    {
        REQUIRE(__ut_ovfl_discard_wrapup(session, &p) == 0);
    }

    __wt_free(session, m.ovfl_track);
}

TEST_CASE(
  "Reconciliation tracking: ovfl_reuse_wrapup restores cache footprint on block_free failure",
  "[reconciliation]")
{
    connection_wrapper conn(DB_HOME);
    WT_SESSION_IMPL *session = conn.create_session();

    WT_BM bm;
    WT_BTREE btree;
    WT_DATA_HANDLE dhandle;
    setup_failing_bm(session, &dhandle, &btree, &bm);

    WT_PAGE page;
    memset(&page, 0, sizeof(page));
    page.type = WT_PAGE_ROW_LEAF;

    WT_PAGE_MODIFY mod;
    memset(&mod, 0, sizeof(mod));
    page.modify = &mod;

    const uint8_t addr = 0xAB;
    REQUIRE(__wti_ovfl_reuse_add(session, &page, &addr, sizeof(addr), "value", 5) == 0);
    REQUIRE(page.memory_footprint > 0);

    F_CLR(mod.ovfl_track->ovfl_reuse[0], WT_OVFL_REUSE_INUSE | WT_OVFL_REUSE_JUST_ADDED);

    int ret = __ut_ovfl_reuse_wrapup(session, &page);

    REQUIRE(ret == EINVAL);

    REQUIRE(page.memory_footprint == 0);

    __wt_ovfl_reuse_free(session, &page);
    __wt_free(session, mod.ovfl_track);
}

TEST_CASE(
  "Reconciliation tracking: ovfl_reuse_wrapup_err propagates block_free error", "[reconciliation]")
{
    connection_wrapper conn(DB_HOME);
    WT_SESSION_IMPL *session = conn.create_session();

    WT_BM bm;
    WT_BTREE btree;
    WT_DATA_HANDLE dhandle;
    setup_failing_bm(session, &dhandle, &btree, &bm);

    WT_PAGE page;
    memset(&page, 0, sizeof(page));
    page.type = WT_PAGE_ROW_LEAF;

    WT_PAGE_MODIFY mod;
    memset(&mod, 0, sizeof(mod));
    page.modify = &mod;

    const uint8_t addr = 0xAB;
    REQUIRE(__wti_ovfl_reuse_add(session, &page, &addr, sizeof(addr), "value", 5) == 0);

    int ret = __ut_ovfl_reuse_wrapup_err(session, &page);

    REQUIRE(ret == EINVAL);
    REQUIRE(page.memory_footprint == 0);

    __wt_ovfl_reuse_free(session, &page);
    __wt_free(session, mod.ovfl_track);
}
