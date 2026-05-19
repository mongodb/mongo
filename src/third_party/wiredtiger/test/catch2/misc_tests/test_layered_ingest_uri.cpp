/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include <string>

#include "wt_internal.h"
#include "../wrappers/mock_session.h"

/*
 * [layered_ingest_uri]: Unit tests for __layered_derive_layered_uri.
 *
 * The function strips the "file:" prefix and ".wt_ingest" suffix from an ingest constituent URI
 * and returns "layered:<name>". It returns EINVAL if the URI doesn't match that shape.
 */

TEST_CASE("layered_derive_layered_uri: valid inputs", "[layered_ingest_uri]")
{
    std::shared_ptr<mock_session> ms = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *session = ms->get_wt_session_impl();

    WT_ITEM buf;
    memset(&buf, 0, sizeof(buf));

    SECTION("simple table name")
    {
        REQUIRE(__ut_layered_derive_layered_uri(session, "file:foo.wt_ingest", &buf) == 0);
        REQUIRE(std::string((const char *)buf.data, buf.size) == "layered:foo");
    }

    SECTION("table name with underscores and digits")
    {
        REQUIRE(__ut_layered_derive_layered_uri(session, "file:my_table_01.wt_ingest", &buf) == 0);
        REQUIRE(std::string((const char *)buf.data, buf.size) == "layered:my_table_01");
    }

    __wt_buf_free(session, &buf);
}

TEST_CASE("layered_derive_layered_uri: invalid inputs", "[layered_ingest_uri]")
{
    std::shared_ptr<mock_session> ms = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *session = ms->get_wt_session_impl();

    WT_ITEM buf;
    memset(&buf, 0, sizeof(buf));

    SECTION("missing file: prefix")
    {
        REQUIRE(__ut_layered_derive_layered_uri(session, "foo.wt_ingest", &buf) == EINVAL);
    }

    SECTION("missing .wt_ingest suffix")
    {
        REQUIRE(__ut_layered_derive_layered_uri(session, "file:foo.wt", &buf) == EINVAL);
    }

    SECTION("wrong prefix, correct suffix")
    {
        REQUIRE(__ut_layered_derive_layered_uri(session, "layered:foo.wt_ingest", &buf) == EINVAL);
    }

    SECTION("stable suffix instead of ingest suffix")
    {
        REQUIRE(__ut_layered_derive_layered_uri(session, "file:foo.wt_stable", &buf) == EINVAL);
    }

    __wt_buf_free(session, &buf);
}
