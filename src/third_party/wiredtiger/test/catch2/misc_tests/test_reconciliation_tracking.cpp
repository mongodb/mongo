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
