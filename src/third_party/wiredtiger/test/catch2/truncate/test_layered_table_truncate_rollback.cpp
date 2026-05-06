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

namespace {

WT_TXN_OP
make_op(WT_TRUNCATE *entry)
{
    WT_TXN_OP op{};
    op.type = WT_TXN_OP_FOLLOWER_TRUNCATE;
    op.u.follower_truncate.t = entry;
    return op;
}

void
rollback_truncate(WT_SESSION_IMPL *session, WT_TRUNCATE *entry)
{
    auto op = make_op(entry);
    __wti_layered_table_truncate_rollback(session, &op);
}

} // namespace

SCENARIO(
  "rolling back a truncate entry removes it from the truncate list", "[truncate_list][rollback]")
{
    GIVEN("a truncate list with one entry")
    {
        truncate_list_fixture fixture;
        fixture.add_entry(make_item("a"), make_item("z"));
        CHECK(truncate_list_size(fixture.layered_table()) == 1);

        WHEN("the truncate is rolled back")
        {
            rollback_truncate(&fixture.session(), truncate_list_head(fixture.layered_table()));

            THEN("the truncate list entry is removed")
            {
                REQUIRE(truncate_list_size(fixture.layered_table()) == 0);
            }
        }
    }
}

SCENARIO("rolling back a truncate entry clears the op pointer", "[truncate_list][rollback]")
{
    GIVEN("a truncate list with one entry")
    {
        truncate_list_fixture fixture;
        fixture.add_entry(make_item("a"), make_item("z"));
        CHECK(truncate_list_size(fixture.layered_table()) == 1);

        WHEN("the truncate is rolled back")
        {
            auto op = make_op(truncate_list_head(fixture.layered_table()));
            __wti_layered_table_truncate_rollback(&fixture.session(), &op);

            THEN("the op pointer is null")
            {
                REQUIRE(op.u.follower_truncate.t == nullptr);
            }
        }
    }
}

SCENARIO(
  "rolling back a truncate entry releases the dhandle reference", "[truncate_list][rollback]")
{
    GIVEN("a truncate list with one entry")
    {
        truncate_list_fixture fixture;
        fixture.add_entry(make_item("a"), make_item("z"));
        CHECK(truncate_list_size(fixture.layered_table()) == 1);

        const auto reference_count = fixture.reference_count();

        WHEN("the truncate is rolled back")
        {
            rollback_truncate(&fixture.session(), truncate_list_head(fixture.layered_table()));

            THEN("the dhandle reference is released")
            {
                REQUIRE(fixture.reference_count() == reference_count - 1u);
            }
        }
    }
}

SCENARIO("rolling back a truncate entry releases the truncate lock", "[truncate_list][rollback]")
{
    GIVEN("a truncate list with one entry")
    {
        truncate_list_fixture fixture;
        fixture.add_entry(make_item("a"), make_item("z"));
        CHECK(truncate_list_size(fixture.layered_table()) == 1);

        WHEN("the truncate is rolled back")
        {
            rollback_truncate(&fixture.session(), truncate_list_head(fixture.layered_table()));

            THEN("the truncate lock is not held")
            {
                REQUIRE(lock_is_released(fixture.session(), fixture.layered_table()));
            }
        }
    }
}

SCENARIO("rolling back affects only the targeted entry in a multi-entry truncate list",
  "[truncate_list][rollback]")
{
    GIVEN("a truncate list with three entries")
    {
        truncate_list_fixture fixture;
        auto *first_entry = fixture.add_entry(make_item("a"), make_item("b"));
        auto *middle_entry = fixture.add_entry(make_item("c"), make_item("d"));
        auto *last_entry = fixture.add_entry(make_item("e"), make_item("f"));

        const auto size = truncate_list_size(fixture.layered_table());
        CHECK(size == 3);

        const auto reference_count = fixture.reference_count();

        auto first_op = make_op(first_entry);
        auto last_op = make_op(last_entry);

        WHEN("the middle truncate is rolled back")
        {
            rollback_truncate(&fixture.session(), middle_entry);

            THEN("only the middle entry is removed and the others remain in order")
            {
                const auto *const head = truncate_list_head(fixture.layered_table());
                REQUIRE(head == first_entry);
                REQUIRE(TAILQ_NEXT(head, q) == last_entry);
                REQUIRE(TAILQ_NEXT(last_entry, q) == nullptr);
            }

            THEN("the other op pointers are not cleared")
            {
                REQUIRE(first_op.u.follower_truncate.t == first_entry);
                REQUIRE(last_op.u.follower_truncate.t == last_entry);
            }

            THEN("the dhandle reference count is unchanged")
            {
                REQUIRE(fixture.reference_count() == reference_count);
            }

            THEN("only one entry is removed")
            {
                const auto expected_size = size - 1u;
                REQUIRE(truncate_list_size(fixture.layered_table()) == expected_size);
            }
        }
    }
}
