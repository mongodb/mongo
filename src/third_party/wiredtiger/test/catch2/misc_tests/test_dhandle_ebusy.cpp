/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * When attempting to lock a dhandle returns EBUSY because the btree has special flags set
 * (bulk/salvage/verify), the session's dhandle ref must be cleared to NULL. Leaving it set exposes
 * callers to a handle they never locked, which can corrupt the dhandle's lock state if they attempt
 * to release it.
 */

#include <catch2/catch.hpp>

#include "wiredtiger.h"
#include "wt_internal.h"
#include "../utils.h"
#include "../wrappers/connection_wrapper.h"
#include "../../utility/test_util.h"

TEST_CASE("session->dhandle is NULL after EBUSY from get_dhandle", "[dhandle][dhandle_ebusy]")
{
    const std::string home = "WT_TEST.dhandle_ebusy";
    testutil_system("rm -rf %s && mkdir -p %s", home.c_str(), home.c_str());

    {
        connection_wrapper conn(home);

        WT_SESSION_IMPL *session_impl = conn.create_session();
        WT_SESSION *session = &session_impl->iface;
        REQUIRE(session->create(session, "file:t.wt", "key_format=i,value_format=i") == 0);

        /*
         * Opening a bulk cursor is one way to set the special flag on the dhandle, which prevents
         * other sessions from locking it.
         */
        WT_CURSOR *bulk = nullptr;
        REQUIRE(session->open_cursor(session, "file:t.wt", nullptr, "bulk", &bulk) == 0);

        WT_SESSION_IMPL *session_impl_b = conn.create_session();
        REQUIRE(session_impl_b->dhandle == nullptr);

        int ret = __wt_session_get_dhandle(session_impl_b, "file:t.wt", nullptr, nullptr, 0);
        REQUIRE(ret == EBUSY);
        CHECK(session_impl_b->dhandle == nullptr);

        REQUIRE(bulk->close(bulk) == 0);
    }

    utils::wiredtiger_cleanup(home);
}
