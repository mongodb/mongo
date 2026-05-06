/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

// External include:
#include <catch2/catch.hpp>

// WiredTiger include:
#include "wt_internal.h"
#include "truncate_list_helpers.hpp"

using namespace truncate_list_helpers;

SCENARIO("clearing empties the truncate list", "[truncate_list][clear]")
{
    GIVEN("a truncate list with two entries")
    {
        truncate_list_fixture fixture;
        fixture.add_entry(make_item("a"), make_item("z"));
        fixture.add_entry(make_item("b"), make_item("y"));
        CHECK(truncate_list_size(fixture.layered_table()) == 2);

        WHEN("the truncate list is cleared")
        {
            __wt_layered_table_truncate_clear(&fixture.session(), &fixture.layered_table());

            THEN("the truncate list entries are removed")
            {
                REQUIRE(truncate_list_size(fixture.layered_table()) == 0);
            }
        }
    }
}

SCENARIO("clearing the truncate list releases the dhandle reference", "[truncate_list][clear]")
{
    GIVEN("a truncate list with two entries")
    {
        truncate_list_fixture fixture;
        fixture.add_entry(make_item("a"), make_item("z"));
        fixture.add_entry(make_item("b"), make_item("y"));
        CHECK(truncate_list_size(fixture.layered_table()) == 2);

        const auto reference_count = fixture.reference_count();

        WHEN("the truncate list is cleared")
        {
            __wt_layered_table_truncate_clear(&fixture.session(), &fixture.layered_table());

            THEN("the dhandle reference is released exactly once")
            {
                REQUIRE(fixture.reference_count() == reference_count - 1u);
            }
        }
    }
}

SCENARIO("clearing an empty truncate list is a no-op", "[truncate_list][clear]")
{
    GIVEN("an empty truncate list")
    {
        truncate_list_fixture fixture;
        const auto reference_count = fixture.reference_count();
        CHECK(truncate_list_size(fixture.layered_table()) == 0);

        WHEN("the truncate list is cleared")
        {
            __wt_layered_table_truncate_clear(&fixture.session(), &fixture.layered_table());

            THEN("the truncate list is empty and the reference count is unchanged")
            {
                REQUIRE(truncate_list_size(fixture.layered_table()) == 0);
                REQUIRE(fixture.reference_count() == reference_count);
            }
        }
    }
}

SCENARIO("clearing the truncate list releases the truncate lock", "[truncate_list][clear]")
{
    GIVEN("a truncate list with two entries")
    {
        truncate_list_fixture fixture;
        fixture.add_entry(make_item("a"), make_item("z"));
        fixture.add_entry(make_item("b"), make_item("y"));
        CHECK(truncate_list_size(fixture.layered_table()) == 2);

        WHEN("the truncate list is cleared")
        {
            __wt_layered_table_truncate_clear(&fixture.session(), &fixture.layered_table());

            THEN("the truncate lock is not held")
            {
                REQUIRE(lock_is_released(fixture.session(), fixture.layered_table()));
            }
        }
    }
}
