/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wt_internal.h"
#include "../../wrappers/mock_metadata_cursor.h"
#include "../../wrappers/mock_session.h"

namespace {

constexpr auto filename = "test.wt";
constexpr auto file_uri = "file:test.wt";
constexpr auto table_uri = "table:test";
constexpr auto stable_uri = "file:test.wt_stable";

int
fs_exist_default(WT_FILE_SYSTEM *, WT_SESSION *, const char *, bool *existp)
{
    *existp = true;
    return 0;
}

int
fs_size_default(WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *sizep)
{
    *sizep = 0;
    return 0;
}

class curstat_size_fixture {
public:
    curstat_size_fixture() : _mock(mock_session::build_test_mock_session())
    {
        const auto connection = _mock->get_mock_connection();
        auto *conn = connection->get_wt_connection_impl();
        auto *session_impl = session();

        REQUIRE(connection->setup_block_manager(session_impl) == 0);

        // Metadata searches need both session and shared transaction state.
        REQUIRE(__wt_calloc_one(session_impl, &session_impl->txn) == 0);
        conn->txn_global.txn_shared_list = &_txn_shared;

        conn->file_system->fs_exist = fs_exist_default;
        conn->file_system->fs_size = fs_size_default;

        // Use the mock cursor for metadata lookups.
        session_impl->meta_cursor = &_metadata_cursor.cursor();
    }

    ~curstat_size_fixture()
    {
        auto *session_impl = session();
        auto *conn = S2C(session_impl);

        // Detach test state before the mock session is destroyed.
        session_impl->meta_cursor = nullptr;
        conn->txn_global.txn_shared_list = nullptr;
        __wt_free(session_impl, session_impl->txn);
        session_impl->txn = nullptr;

        // Reset optional disaggregated configuration.
        conn->disaggregated_storage.page_log_meta = nullptr;
        __wt_conn_config_discard(session_impl);
    }

    void
    enable_disaggregated_storage()
    {
        auto *session_impl = session();

        // File classification needs the default metadata configuration.
        REQUIRE(__wt_conn_config_init(session_impl) == 0);

        // A metadata page log handle marks the connection as disaggregated.
        S2C(session_impl)->disaggregated_storage.page_log_meta = &_page_log_handle;
    }

    [[nodiscard]] WT_FILE_SYSTEM &
    file_system()
    {
        return *S2C(session())->file_system;
    }

    [[nodiscard]] mock_metadata_cursor &
    metadata_cursor()
    {
        return _metadata_cursor;
    }

    [[nodiscard]] WT_SESSION_IMPL *
    session() const
    {
        return _mock->get_wt_session_impl();
    }

private:
    std::shared_ptr<mock_session> _mock;
    mock_metadata_cursor _metadata_cursor;
    WT_TXN_SHARED _txn_shared{};
    WT_PAGE_LOG_HANDLE _page_log_handle{};
};

} // namespace

TEST_CASE_METHOD(curstat_size_fixture, "local size", "[curstat_size]")
{
    SECTION("propagates a file system error")
    {
        constexpr auto expected_error = EIO;
        file_system().fs_exist = [](WT_FILE_SYSTEM *, WT_SESSION *, const char *, bool *) {
            return EIO;
        };

        bool was_fast = true;
        int64_t size = 0;
        const auto result = __ut_curstat_size_local(session(), filename, &was_fast, &size);

        REQUIRE(result == expected_error);
    }

    SECTION("reports a missing file")
    {
        file_system().fs_exist = [](WT_FILE_SYSTEM *, WT_SESSION *, const char *, bool *existp) {
            *existp = false;
            return 0;
        };

        bool was_fast = true;
        int64_t size = 0;
        const auto result = __ut_curstat_size_local(session(), filename, &was_fast, &size);

        REQUIRE(result == 0);
        REQUIRE_FALSE(was_fast);
    }

    SECTION("propagates a size error")
    {
        constexpr auto expected_error = EIO;
        file_system().fs_size = [](WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *) {
            return EIO;
        };

        bool was_fast = true;
        int64_t size = 0;
        const auto result = __ut_curstat_size_local(session(), filename, &was_fast, &size);

        REQUIRE(result == expected_error);
    }

    SECTION("treats a concurrent removal as missing")
    {
        file_system().fs_size = [](WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *) {
            return ENOENT;
        };

        bool was_fast = true;
        int64_t size = 0;
        const auto result = __ut_curstat_size_local(session(), filename, &was_fast, &size);

        REQUIRE(result == 0);
        REQUIRE_FALSE(was_fast);
    }

    SECTION("reports a valid size")
    {
        constexpr wt_off_t expected_size = 5678;
        file_system().fs_size = [](WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *sizep) {
            *sizep = 5678;
            return 0;
        };

        bool was_fast = false;
        int64_t size = 0;
        const auto result = __ut_curstat_size_local(session(), filename, &was_fast, &size);

        REQUIRE(result == 0);
        REQUIRE(was_fast);
        REQUIRE(size == expected_size);
    }
}

TEST_CASE_METHOD(curstat_size_fixture, "shared size", "[curstat_size]")
{
    SECTION("reports zero when there is no checkpoint entry")
    {
        constexpr auto config = "checkpoint=()";

        int64_t size = 1;
        const auto result = __ut_curstat_size_shared(session(), config, &size);

        REQUIRE(result == 0);
        REQUIRE(size == 0);
    }

    SECTION("reports a zero-sized checkpoint")
    {
        constexpr auto config =
          "checkpoint=(WiredTigerCheckpoint.1=(addr=\"\",order=1,time=1,size=0,write_gen=1))";

        int64_t size = 1;
        const auto result = __ut_curstat_size_shared(session(), config, &size);

        REQUIRE(result == 0);
        REQUIRE(size == 0);
    }

    SECTION("reports a nonzero checkpoint size")
    {
        constexpr int64_t expected_size = 5678;
        constexpr auto config =
          "checkpoint=(WiredTigerCheckpoint.1=(addr=\"\",order=1,time=1,size=5678,write_gen=1))";

        int64_t size = 0;
        const auto result = __ut_curstat_size_shared(session(), config, &size);

        REQUIRE(result == 0);
        REQUIRE(size == expected_size);
    }
}

TEST_CASE_METHOD(curstat_size_fixture, "file size", "[curstat_size][curstat_file_size]")
{
    SECTION("propagates a metadata search error")
    {
        enable_disaggregated_storage();

        constexpr auto expected_error = EIO;
        metadata_cursor().insert_metadata_error(file_uri, expected_error);

        bool was_fast = false;
        int64_t size = 0;
        const auto result = __ut_curstat_file_size(session(), file_uri, &was_fast, &size);

        REQUIRE(result == expected_error);
    }

    SECTION("propagates a block manager classification error")
    {
        enable_disaggregated_storage();

        constexpr auto expected_error = EINVAL;
        constexpr auto file_config = "block_manager=(";
        metadata_cursor().insert_metadata(file_uri, file_config);

        bool was_fast = false;
        int64_t size = 0;
        const auto result = __ut_curstat_file_size(session(), file_uri, &was_fast, &size);

        REQUIRE(result == expected_error);
    }

    SECTION("retrieves the shared checkpoint size")
    {
        enable_disaggregated_storage();

        constexpr int64_t expected_size = 5678;
        constexpr auto file_config =
          "block_manager=disagg,"
          "checkpoint=(WiredTigerCheckpoint.1=("
          "addr=\"\",order=1,time=1,size=5678,write_gen=1))";
        metadata_cursor().insert_metadata(file_uri, file_config);

        bool was_fast = false;
        int64_t size = 0;
        const auto result = __ut_curstat_file_size(session(), file_uri, &was_fast, &size);

        REQUIRE(result == 0);
        REQUIRE(was_fast);
        REQUIRE(size == expected_size);
    }

    SECTION("propagates an error from local sizing")
    {
        constexpr auto expected_error = EIO;
        file_system().fs_size = [](WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *) {
            return EIO;
        };

        bool was_fast = false;
        int64_t size = 0;
        const auto result = __ut_curstat_file_size(session(), file_uri, &was_fast, &size);

        REQUIRE(result == expected_error);
    }

    SECTION("retrieves the local file size")
    {
        constexpr wt_off_t expected_size = 4321;
        file_system().fs_size = [](WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *sizep) {
            *sizep = 4321;
            return 0;
        };

        bool was_fast = false;
        int64_t size = 0;
        const auto result = __ut_curstat_file_size(session(), file_uri, &was_fast, &size);

        REQUIRE(result == 0);
        REQUIRE(was_fast);
        REQUIRE(size == expected_size);
    }
}

TEST_CASE_METHOD(curstat_size_fixture, "table size", "[curstat_size][curstat_table_size]")
{
    SECTION("reports missing table metadata")
    {
        bool was_fast = false;
        int64_t size = 0;
        const auto result = __ut_curstat_table_size(session(), table_uri, &was_fast, &size);

        REQUIRE(result == WT_NOTFOUND);
    }

    SECTION("propagates a metadata search error")
    {
        constexpr auto expected_error = EIO;
        metadata_cursor().insert_metadata_error(table_uri, expected_error);

        bool was_fast = false;
        int64_t size = 0;
        const auto result = __ut_curstat_table_size(session(), table_uri, &was_fast, &size);

        REQUIRE(result == expected_error);
    }

    SECTION("propagates a columns lookup error")
    {
        metadata_cursor().insert_metadata(table_uri, "key_format=S");

        bool was_fast = false;
        int64_t size = 0;
        const auto result = __ut_curstat_table_size(session(), table_uri, &was_fast, &size);

        REQUIRE(result == WT_NOTFOUND);
    }

    SECTION("propagates a simple-table classification error")
    {
        metadata_cursor().insert_metadata(table_uri, "columns=\"(\"");

        bool was_fast = false;
        int64_t size = 0;
        const auto result = __ut_curstat_table_size(session(), table_uri, &was_fast, &size);

        REQUIRE(result == EINVAL);
    }

    SECTION("skips the fast path for a non-simple table")
    {
        metadata_cursor().insert_metadata(table_uri, "columns=(key,value)");

        bool was_fast = true;
        int64_t size = 0;
        const auto result = __ut_curstat_table_size(session(), table_uri, &was_fast, &size);

        REQUIRE(result == 0);
        REQUIRE_FALSE(was_fast);
    }

    SECTION("propagates an error from the backing file size lookup")
    {
        metadata_cursor().insert_metadata(table_uri, "columns=()");

        constexpr auto expected_error = EIO;
        file_system().fs_size = [](WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *) {
            return EIO;
        };

        bool was_fast = false;
        int64_t size = 0;
        const auto result = __ut_curstat_table_size(session(), table_uri, &was_fast, &size);

        REQUIRE(result == expected_error);
    }

    SECTION("retrieves its backing file size")
    {
        metadata_cursor().insert_metadata(table_uri, "columns=()");

        constexpr wt_off_t expected_size = 4321;
        file_system().fs_size = [](WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *sizep) {
            *sizep = 4321;
            return 0;
        };

        bool was_fast = false;
        int64_t size = 0;
        const auto result = __ut_curstat_table_size(session(), table_uri, &was_fast, &size);

        REQUIRE(result == 0);
        REQUIRE(was_fast);
        REQUIRE(size == expected_size);
    }

    SECTION("propagates a stable metadata search error")
    {
        enable_disaggregated_storage();

        metadata_cursor().insert_metadata(table_uri, "columns=()");

        constexpr auto expected_error = EIO;
        metadata_cursor().insert_metadata_error(stable_uri, expected_error);

        bool was_fast = false;
        int64_t size = 0;
        const auto result = __ut_curstat_table_size(session(), table_uri, &was_fast, &size);

        REQUIRE(result == expected_error);
    }

    SECTION("retrieves the shared checkpoint size")
    {
        enable_disaggregated_storage();

        metadata_cursor().insert_metadata(table_uri, "columns=()");

        constexpr int64_t expected_size = 5678;
        constexpr auto stable_config =
          "checkpoint=(WiredTigerCheckpoint.1=("
          "addr=\"\",order=1,time=1,size=5678,write_gen=1))";

        metadata_cursor().insert_metadata(stable_uri, stable_config);

        bool was_fast = false;
        int64_t size = 0;
        const auto result = __ut_curstat_table_size(session(), table_uri, &was_fast, &size);

        REQUIRE(result == 0);
        REQUIRE(was_fast);
        REQUIRE(size == expected_size);
    }

    SECTION("defers to the slow path when stable and local file metadata are missing")
    {
        enable_disaggregated_storage();

        metadata_cursor().insert_metadata(table_uri, "columns=()");

        bool was_fast = true;
        int64_t size = 0;
        const auto result = __ut_curstat_table_size(session(), table_uri, &was_fast, &size);

        REQUIRE(result == 0);
        REQUIRE_FALSE(was_fast);
    }

    SECTION("falls back to its local backing file when stable metadata is missing")
    {
        enable_disaggregated_storage();

        metadata_cursor().insert_metadata(table_uri, "columns=()");

        // Mark the fallback file as local on the disaggregated connection.
        metadata_cursor().insert_metadata(file_uri, "block_manager=default");

        constexpr wt_off_t expected_size = 4321;
        file_system().fs_size = [](WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *sizep) {
            *sizep = 4321;
            return 0;
        };

        bool was_fast = false;
        int64_t size = 0;
        const auto result = __ut_curstat_table_size(session(), table_uri, &was_fast, &size);

        REQUIRE(result == 0);
        REQUIRE(was_fast);
        REQUIRE(size == expected_size);
    }
}
