/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "utils.h"
#include "wiredtiger.h"
#include "wrappers/connection_wrapper.h"
#include "wt_internal.h"

TEST_CASE("Reconciliation tracking: ovfl_track_init", "[reconciliation]")
{
    ConnectionWrapper conn(DB_HOME);
    WT_SESSION_IMPL *session = conn.createSession();

    WT_PAGE p;
    memset(&p, 0, sizeof(p));

    WT_PAGE_MODIFY m;
    memset(&m, 0, sizeof(m));

    p.modify = &m;

    REQUIRE(__wt_ovfl_track_init(session, &p) == 0);
    REQUIRE(m.ovfl_track != nullptr);
}

TEST_CASE("Reconciliation tracking: ovfl_discard_verbose", "[reconciliation]")
{
    ConnectionWrapper conn(DB_HOME);
    WT_SESSION_IMPL *session = conn.createSession();

    SECTION("handle null page and tag")
    {
        REQUIRE(__ut_ovfl_discard_verbose(session, nullptr, nullptr, nullptr) == 0);
    }
}

TEST_CASE("Reconciliation tracking: ovfl_discard_wrapup", "[reconciliation]")
{
    ConnectionWrapper conn(DB_HOME);
    WT_SESSION_IMPL *session = conn.createSession();

    WT_PAGE p;
    memset(&p, 0, sizeof(p));

    WT_PAGE_MODIFY m;
    memset(&m, 0, sizeof(m));
    p.modify = &m;

    REQUIRE(__wt_ovfl_track_init(session, &p) == 0);

    SECTION("handle empty overflow entry list")
    {
        REQUIRE(__ut_ovfl_discard_wrapup(session, &p) == 0);
    }
}
