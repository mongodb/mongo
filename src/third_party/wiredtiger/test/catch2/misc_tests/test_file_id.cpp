/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wt_internal.h"
#include "wrappers/mock_session.h"

namespace {

class file_id_fixture {
public:
    file_id_fixture()
    {
        REQUIRE(__wt_spin_init(session, &connection->schema_lock, "schema") == 0);
    }

    ~file_id_fixture()
    {
        __wt_spin_destroy(session, &connection->schema_lock);
    }

private:
    std::shared_ptr<mock_session> _mock{mock_session::build_test_mock_session()};

protected:
    static constexpr uint32_t max_file_id = UINT32_MAX >> WT_BTREE_ID_NAMESPACE_BITS;
    WT_SESSION_IMPL *session{_mock->get_wt_session_impl()};
    WT_CONNECTION_IMPL *connection{_mock->get_mock_connection()->get_wt_connection_impl()};
};

} // namespace

SCENARIO_METHOD(file_id_fixture, "File IDs: allocation increments the counter", "[file_id]")
{
    GIVEN("a connection with file IDs available")
    {
        connection->next_file_id = 0;

        WHEN("allocating a file ID")
        {
            uint32_t file_id = 0;
            const auto result = __wt_generate_file_id(session, "file:test.wt", false, &file_id);

            THEN("the return code is zero")
            {
                REQUIRE(result == 0);
            }

            THEN("the file ID counter increments by one")
            {
                REQUIRE(connection->next_file_id == 1);
            }
        }
    }
}

SCENARIO_METHOD(file_id_fixture, "File IDs: allocation rejects overflow", "[file_id]")
{
    GIVEN("a connection with one file ID remaining")
    {
        connection->next_file_id = max_file_id - 1;

        WHEN("allocating a file ID")
        {
            uint32_t file_id = 0;
            const auto result = __wt_generate_file_id(session, "file:test.wt", false, &file_id);

            THEN("the return code is zero")
            {
                REQUIRE(result == 0);
            }

            THEN("the assigned ID is the last valid file ID")
            {
                const auto expected =
                  WT_BTREE_ID_NAMESPACED(max_file_id, WT_BTREE_ID_NAMESPACE_LOCAL);

                REQUIRE(file_id == expected);
            }
        }
    }

    GIVEN("a connection whose file ID counter is at or beyond the namespace boundary")
    {
        const auto next_file_ids = {max_file_id, max_file_id + 1};
        const auto next_file_id = GENERATE_COPY(values<uint32_t>(next_file_ids));

        CAPTURE(next_file_id);
        connection->next_file_id = next_file_id;

        WHEN("allocating a file ID")
        {
            uint32_t file_id = UINT32_MAX;
            const auto result = __wt_generate_file_id(session, "file:test.wt", false, &file_id);

            THEN("the return code is WT_ERROR")
            {
                REQUIRE(result == WT_ERROR);
            }
        }
    }
}
