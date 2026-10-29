/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <filesystem>
#include <catch2/catch.hpp>

#include "wt_internal.h"
#include "wrappers/connection_wrapper.h"

namespace {

class layered_table_manager_fixture {
public:
    layered_table_manager_fixture()
    {
        std::filesystem::remove_all(home);

        constexpr auto conn_config =
          "create,"
          "extensions=[./ext/page_log/palite/libwiredtiger_palite.so],"
          "disaggregated=(role=follower,page_log=palite)";

        _conn = new connection_wrapper(home, conn_config);
        _session = _conn->create_session();
    }

    ~layered_table_manager_fixture()
    {
        delete _conn;
        std::filesystem::remove_all(home);
    }

    WT_LAYERED_TABLE *
    make_layered_table() const
    {
        constexpr auto uri = "layered:test_table_manager";
        constexpr auto config = "key_format=S,value_format=S,block_manager=disagg,type=layered";

        // Make the table.
        auto *iface = &_session->iface;
        CHECK(iface->create(iface, uri, config) == 0);

        // Return that table's dhandle.
        CHECK(__wt_session_get_dhandle(_session, uri, nullptr, nullptr, 0) == 0);
        auto *layered_table = reinterpret_cast<WT_LAYERED_TABLE *>(_session->dhandle);
        CHECK(__wt_session_release_dhandle(_session) == 0);

        return layered_table;
    }

    WT_LAYERED_TABLE_MANAGER *
    manager() const
    {
        return &S2C(_session)->layered_table_manager;
    }

private:
    static constexpr auto home = "WT_TEST.layered_table_manager";

    connection_wrapper *_conn;
    WT_SESSION_IMPL *_session;
};

} // namespace

SCENARIO("Layered table manager uses the unnamespaced ingest ID as the entry key",
  "[layered_table_manager]")
{
    GIVEN("A layered table manager")
    {
        layered_table_manager_fixture f;
        auto *manager = f.manager();

        WHEN("A layered table is opened")
        {
            auto *layered_table = f.make_layered_table();

            THEN("The manager uses the unnamespaced ingest ID as the entry key")
            {
                const auto ingest_id = layered_table->ingest_btree_id;
                const auto slot = WT_BTREE_ID_UNNAMESPACED(ingest_id);

                REQUIRE(manager->entries[slot] != nullptr);
                REQUIRE(manager->entries[slot]->ingest_id == ingest_id);
            }
        }
    }
}
