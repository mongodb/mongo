/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <tuple>

#include <catch2/catch.hpp>

#include "wt_internal.h"
#include "wrappers/connection_wrapper.h"
#include "truncate_list_helpers.hpp"

using namespace truncate_list_helpers;

namespace {

// Converts an integer to a string in the form "key" + "NNN", where NNN is zero-padded.
std::string
format_key(const int num)
{
    std::ostringstream oss;
    oss << "key" << std::setfill('0') << std::setw(3) << num;
    return oss.str();
}

// Calls the provided functor/lambda, wrapped in a transaction.
template <typename Op>
int
do_in_committed_transaction(WT_SESSION_IMPL *session, const Op operation)
{
    auto *iface = &session->iface;

    CHECK(iface->begin_transaction(iface, nullptr) == 0);
    const int ret = operation();
    CHECK(iface->commit_transaction(iface, nullptr) == 0);

    return ret;
}

// Calls the provided functor/lambda in a transaction, but rolls back afterwards.
template <typename Op>
int
do_in_rolled_back_transaction(WT_SESSION_IMPL *session, const Op operation)
{
    auto *iface = &session->iface;

    CHECK(iface->begin_transaction(iface, nullptr) == 0);
    const int ret = operation();
    CHECK(iface->rollback_transaction(iface, nullptr) == 0);

    return ret;
}

// Calls the provided functor/lambda, but leaves the transaction open.
template <typename Op>
int
do_in_uncommitted_transaction(WT_SESSION_IMPL *session, const Op operation)
{
    auto *iface = &session->iface;
    CHECK(iface->begin_transaction(iface, nullptr) == 0);
    return operation();
}

void
set_read_timestamp(WT_SESSION_IMPL *session, const wt_timestamp_t ts)
{
    WT_SESSION_TXN_SHARED(session)->read_timestamp = ts;
    F_SET(session->txn, WT_TXN_SHARED_TS_READ);
}

// Scope guard that ensures that transactions are always rolled back.
[[nodiscard]] auto
rollback_on_exit(WT_SESSION_IMPL *session)
{
    auto *iface = &session->iface;
    const auto deleter = [](WT_SESSION *s) { std::ignore = s->rollback_transaction(s, nullptr); };

    return std::unique_ptr<WT_SESSION, decltype(deleter)>(iface, deleter);
}

// Ensures the given test directory is removed between tests.
class home_directory {
public:
    explicit home_directory(const std::string_view path) : _path(path)
    {
        std::filesystem::remove_all(path);
    }

    ~home_directory()
    {
        std::filesystem::remove_all(_path);
    }

    [[nodiscard]] const char *
    path() const
    {
        return _path.c_str();
    }

private:
    std::string _path;
};

class write_conflict_fixture {
public:
    write_conflict_fixture()
    {
        constexpr auto uri = "layered:write_conflict";

        static constexpr auto config =
          "key_format=S,value_format=S,block_manager=disagg,type=layered";

        auto &session = _session->iface;
        CHECK(session.create(&session, uri, config) == 0);
        CHECK(session.open_cursor(&session, uri, nullptr, nullptr, &_cursor) == 0);
    }

    [[nodiscard]] WT_SESSION_IMPL *
    session() const
    {
        return _session;
    }

    [[nodiscard]] WT_SESSION_IMPL *
    create_session()
    {
        return _conn.create_session();
    }

    [[nodiscard]] WT_LAYERED_TABLE *
    layered_table() const
    {
        auto *layered_cursor = reinterpret_cast<WT_CURSOR_LAYERED *>(_cursor);
        return reinterpret_cast<WT_LAYERED_TABLE *>(layered_cursor->dhandle);
    }

    int
    insert_truncate_entry(WT_SESSION_IMPL *session, const int start, const int stop)
    {
        const auto start_str = format_key(start);
        const auto stop_str = format_key(stop);
        auto start_item = make_item(start_str);
        auto stop_item = make_item(stop_str);

        return __wt_insert_truncate_entry(session, layered_table(), &start_item, &stop_item);
    }

    int
    detect_conflict(WT_SESSION_IMPL *session, const int key)
    {
        const auto key_str = format_key(key);
        auto key_item = make_item(key_str);

        return __wt_layered_table_truncate_detect_write_conflict(
          session, layered_table(), &key_item);
    }

    int
    detect_non_ingest_conflict(WT_SESSION_IMPL *session, const int start, const int stop)
    {
        const auto start_str = format_key(start);
        const auto stop_str = format_key(stop);
        auto start_item = make_item(start_str);
        auto stop_item = make_item(stop_str);

        return __wt_layered_table_truncate_detect_non_ingest_write_conflict(
          session, layered_table(), &start_item, &stop_item);
    }

private:
    static constexpr auto conn_config =
      "create,"
      "extensions=[./ext/page_log/palite/libwiredtiger_palite.so],"
      "disaggregated=(role=follower,page_log=palite)";

    home_directory _home{"WT_TEST.truncate_write_conflict"};
    connection_wrapper _conn{_home.path(), conn_config};
    WT_SESSION_IMPL *_session{_conn.create_session()};
    WT_CURSOR *_cursor{};
};

} // namespace

SCENARIO("write conflict returns 0 for an empty truncate list", "[truncate_list][write_conflict]")
{
    GIVEN("a layered table with an empty truncate list")
    {
        write_conflict_fixture f;

        WHEN("the conflict check is called for any key")
        {
            const auto result = do_in_rolled_back_transaction(
              f.session(), [&] { return f.detect_conflict(f.session(), 150); });

            THEN("it returns 0 (no conflict)")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO("write conflict returns 0 when the key is outside all uncommitted ranges",
  "[truncate_list][write_conflict]")
{
    GIVEN("one uncommitted truncate range [100, 200]")
    {
        write_conflict_fixture f;

        do_in_uncommitted_transaction(
          f.session(), [&] { return f.insert_truncate_entry(f.session(), 100, 200); });

        const auto cleanup = rollback_on_exit(f.session());

        WHEN("the conflict check is called for a key before the range")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_conflict(session_2, 50); });

            THEN("it returns 0 (no conflict)")
            {
                REQUIRE(result == 0);
            }
        }

        WHEN("the conflict check is called for a key after the range")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_conflict(session_2, 250); });

            THEN("it returns 0 (no conflict)")
            {
                REQUIRE(result == 0);
            }
        }
    }

    GIVEN("two non-overlapping uncommitted ranges [100, 200] and [400, 500]")
    {
        write_conflict_fixture f;

        do_in_uncommitted_transaction(f.session(), [&] {
            CHECK(f.insert_truncate_entry(f.session(), 100, 200) == 0);
            CHECK(f.insert_truncate_entry(f.session(), 400, 500) == 0);
            return 0;
        });

        const auto cleanup = rollback_on_exit(f.session());

        WHEN("the conflict check is called for a key between the ranges")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_conflict(session_2, 300); });

            THEN("it returns 0 (no conflict)")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO("write conflict returns WT_ROLLBACK when the key is inside an uncommitted range",
  "[truncate_list][write_conflict]")
{
    GIVEN("one uncommitted truncate range [100, 200]")
    {
        write_conflict_fixture f;

        do_in_uncommitted_transaction(
          f.session(), [&] { return f.insert_truncate_entry(f.session(), 100, 200); });

        const auto cleanup = rollback_on_exit(f.session());

        WHEN("the conflict check is called for a key strictly inside the range")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_conflict(session_2, 150); });

            THEN("it returns WT_ROLLBACK")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called for the start boundary key")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_conflict(session_2, 100); });

            THEN("it returns WT_ROLLBACK (start boundary is inclusive)")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called for the stop boundary key")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_conflict(session_2, 200); });

            THEN("it returns WT_ROLLBACK (stop boundary is inclusive)")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }
    }
}

SCENARIO("write conflict with a single-key uncommitted range", "[truncate_list][write_conflict]")
{
    GIVEN("a single-key uncommitted range [100, 100]")
    {
        write_conflict_fixture f;

        do_in_uncommitted_transaction(
          f.session(), [&] { return f.insert_truncate_entry(f.session(), 100, 100); });

        const auto cleanup = rollback_on_exit(f.session());

        WHEN("the conflict check is called for the exact key")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_conflict(session_2, 100); });

            THEN("it returns WT_ROLLBACK")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called for a key just before the range")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_conflict(session_2, 99); });

            THEN("it returns 0 (no conflict)")
            {
                REQUIRE(result == 0);
            }
        }

        WHEN("the conflict check is called for a key just after the range")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_conflict(session_2, 101); });

            THEN("it returns 0 (no conflict)")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO(
  "write conflict with two non-overlapping uncommitted ranges", "[truncate_list][write_conflict]")
{
    GIVEN("uncommitted ranges [100, 200] and [400, 500]")
    {
        write_conflict_fixture f;

        do_in_uncommitted_transaction(f.session(), [&] {
            CHECK(f.insert_truncate_entry(f.session(), 100, 200) == 0);
            CHECK(f.insert_truncate_entry(f.session(), 400, 500) == 0);
            return 0;
        });

        const auto cleanup = rollback_on_exit(f.session());

        WHEN("the conflict check is called for a key in the first range")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_conflict(session_2, 150); });

            THEN("it returns WT_ROLLBACK")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called for a key in the second range")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_conflict(session_2, 450); });

            THEN("it returns WT_ROLLBACK")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }
    }
}

SCENARIO("write conflict does not trigger for a committed truncate range",
  "[truncate_list][write_conflict]")
{
    GIVEN("one committed (globally visible) truncate range [100, 200]")
    {
        write_conflict_fixture f;

        do_in_committed_transaction(
          f.session(), [&] { return f.insert_truncate_entry(f.session(), 100, 200); });

        WHEN("the conflict check is called for a key inside the committed range")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_conflict(session_2, 150); });

            THEN("it returns 0 (no conflict)")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO("write conflict does not trigger for the caller's own uncommitted range",
  "[truncate_list][write_conflict]")
{
    GIVEN("an uncommitted truncate range owned by the current transaction")
    {
        write_conflict_fixture f;

        WHEN("the conflict check is called for a key inside that range")
        {
            const auto result = do_in_rolled_back_transaction(f.session(), [&] {
                CHECK(f.insert_truncate_entry(f.session(), 100, 200) == 0);
                return f.detect_conflict(f.session(), 150);
            });

            THEN("it returns 0 (no self-conflict)")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO("write conflict with overlapping committed and uncommitted ranges",
  "[truncate_list][write_conflict]")
{
    GIVEN("a committed range [100, 300] and an uncommitted range [200, 400]")
    {
        write_conflict_fixture f;

        do_in_committed_transaction(
          f.session(), [&] { return f.insert_truncate_entry(f.session(), 100, 300); });

        auto *session_2 = f.create_session();

        do_in_uncommitted_transaction(
          session_2, [&] { return f.insert_truncate_entry(session_2, 200, 400); });

        const auto cleanup = rollback_on_exit(session_2);

        WHEN("the conflict check is called for a key covered only by the committed range")
        {
            auto *session_3 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_3, [&] { return f.detect_conflict(session_3, 150); });

            THEN("it returns 0 (no conflict)")
            {
                REQUIRE(result == 0);
            }
        }

        WHEN("the conflict check is called for a key in the overlap region")
        {
            auto *session_3 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_3, [&] { return f.detect_conflict(session_3, 250); });

            THEN("it returns WT_ROLLBACK (uncommitted range covers the key)")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called for a key covered only by the uncommitted range")
        {
            auto *session_3 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_3, [&] { return f.detect_conflict(session_3, 350); });

            THEN("it returns WT_ROLLBACK")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called for the start boundary of the committed range")
        {
            auto *session_3 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_3, [&] { return f.detect_conflict(session_3, 100); });

            THEN("it returns 0 (committed range is visible; key is not in uncommitted range)")
            {
                REQUIRE(result == 0);
            }
        }

        WHEN("the conflict check is called for the start boundary of the uncommitted range")
        {
            auto *session_3 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_3, [&] { return f.detect_conflict(session_3, 200); });

            THEN("it returns WT_ROLLBACK (uncommitted range covers the key)")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called for the stop boundary of the committed range")
        {
            auto *session_3 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_3, [&] { return f.detect_conflict(session_3, 300); });

            THEN("it returns WT_ROLLBACK (uncommitted range covers the key)")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called for the stop boundary of the uncommitted range")
        {
            auto *session_3 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_3, [&] { return f.detect_conflict(session_3, 400); });

            THEN("it returns WT_ROLLBACK (uncommitted range covers the key)")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }
    }
}

SCENARIO("write conflict read lock is always released", "[truncate_list][write_conflict]")
{
    GIVEN("a layered table")
    {
        write_conflict_fixture f;

        WHEN("the conflict check is called")
        {
            do_in_rolled_back_transaction(
              f.session(), [&] { return f.detect_conflict(f.session(), 150); });

            THEN("the truncate lock is not held")
            {
                REQUIRE(lock_is_released(*f.session(), *f.layered_table()));
            }
        }
    }
}

SCENARIO("write conflict returns WT_ROLLBACK when read_timestamp predates a committed truncate",
  "[truncate_list][write_conflict]")
{
    GIVEN("a committed truncate range [100, 200] with timestamp 30")
    {
        write_conflict_fixture f;

        do_in_committed_transaction(
          f.session(), [&] { return f.insert_truncate_entry(f.session(), 100, 200); });

        WT_TRUNCATE *entry = truncate_list_head(*f.layered_table());
        entry->start_ts = 30;
        entry->durable_ts = 30;

        WHEN("the conflict check is called with read_timestamp before the truncate timestamp")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(session_2, [&] {
                set_read_timestamp(session_2, 29);
                return f.detect_conflict(session_2, 150);
            });

            THEN("it returns WT_ROLLBACK (truncate is not visible at this read timestamp)")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called with read_timestamp equal to the truncate timestamp")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(session_2, [&] {
                set_read_timestamp(session_2, 30);
                return f.detect_conflict(session_2, 150);
            });

            THEN("it returns 0 (truncate is visible at this read timestamp)")
            {
                REQUIRE(result == 0);
            }
        }

        WHEN("the conflict check is called with read_timestamp after the truncate timestamp")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(session_2, [&] {
                set_read_timestamp(session_2, 31);
                return f.detect_conflict(session_2, 150);
            });

            THEN("it returns 0 (truncate is visible at this read timestamp)")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO("non-ingest write conflict returns 0 for an empty truncate list",
  "[truncate_list][write_conflict]")
{
    GIVEN("a layered table with an empty truncate list")
    {
        write_conflict_fixture f;

        WHEN("the non-ingest conflict check is called for any range")
        {
            const auto result = do_in_rolled_back_transaction(
              f.session(), [&] { return f.detect_non_ingest_conflict(f.session(), 100, 200); });

            THEN("it returns 0 (no conflict)")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO("non-ingest write conflict returns 0 when ranges do not overlap",
  "[truncate_list][write_conflict]")
{
    GIVEN("one uncommitted truncate range [100, 200]")
    {
        write_conflict_fixture f;

        do_in_uncommitted_transaction(
          f.session(), [&] { return f.insert_truncate_entry(f.session(), 100, 200); });

        const auto cleanup = rollback_on_exit(f.session());

        WHEN("the new range ends just before the existing start")
        {
            auto *session_2 = f.create_session();

            // new [10, 99]: 99 < 100 so neither overlap check fires
            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_non_ingest_conflict(session_2, 10, 99); });

            THEN("it returns 0 (no conflict)")
            {
                REQUIRE(result == 0);
            }
        }

        WHEN("the new range starts just after the existing stop")
        {
            auto *session_2 = f.create_session();

            // new [201, 400]: 201 > 200 so neither overlap check fires
            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_non_ingest_conflict(session_2, 201, 400); });

            THEN("it returns 0 (no conflict)")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO(
  "non-ingest write conflict returns WT_ROLLBACK when ranges overlap with an uncommitted truncate",
  "[truncate_list][write_conflict]")
{
    GIVEN("one uncommitted truncate range [100, 300]")
    {
        write_conflict_fixture f;

        do_in_uncommitted_transaction(
          f.session(), [&] { return f.insert_truncate_entry(f.session(), 100, 300); });

        const auto cleanup = rollback_on_exit(f.session());

        WHEN("the new range starts at the existing stop boundary")
        {
            auto *session_2 = f.create_session();

            // new [300, 400]: new_start(300) falls within existing [100, 300]
            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_non_ingest_conflict(session_2, 300, 400); });

            THEN("it returns WT_ROLLBACK (stop boundary is inclusive)")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the new range ends at the existing start boundary")
        {
            auto *session_2 = f.create_session();

            // new [50, 100]: existing_start(100) falls within new [50, 100]
            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_non_ingest_conflict(session_2, 50, 100); });

            THEN("it returns WT_ROLLBACK (start boundary is inclusive)")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }
    }
}

SCENARIO("non-ingest write conflict does not trigger for a committed truncate range",
  "[truncate_list][write_conflict]")
{
    GIVEN("one committed (globally visible) truncate range [100, 300]")
    {
        write_conflict_fixture f;

        do_in_committed_transaction(
          f.session(), [&] { return f.insert_truncate_entry(f.session(), 100, 300); });

        WHEN("the non-ingest conflict check is called for an overlapping range")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_non_ingest_conflict(session_2, 200, 400); });

            THEN("it returns 0 (no conflict)")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO("non-ingest write conflict does not trigger for the caller's own uncommitted range",
  "[truncate_list][write_conflict]")
{
    GIVEN("an uncommitted truncate range owned by the current transaction")
    {
        write_conflict_fixture f;

        WHEN(
          "the non-ingest conflict check is called for an overlapping range in the same "
          "transaction")
        {
            const auto result = do_in_rolled_back_transaction(f.session(), [&] {
                CHECK(f.insert_truncate_entry(f.session(), 100, 300) == 0);
                return f.detect_non_ingest_conflict(f.session(), 200, 400);
            });

            THEN("it returns 0 (no self-conflict)")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO(
  "non-ingest write conflict read lock is always released", "[truncate_list][write_conflict]")
{
    GIVEN("a layered table")
    {
        write_conflict_fixture f;

        WHEN("the non-ingest conflict check is called")
        {
            do_in_rolled_back_transaction(
              f.session(), [&] { return f.detect_non_ingest_conflict(f.session(), 100, 200); });

            THEN("the truncate lock is not held")
            {
                REQUIRE(lock_is_released(*f.session(), *f.layered_table()));
            }
        }
    }
}

SCENARIO(
  "non-ingest write conflict returns WT_ROLLBACK when read_timestamp predates a committed truncate",
  "[truncate_list][write_conflict]")
{
    GIVEN("a committed truncate range [100, 300] with timestamp 30")
    {
        write_conflict_fixture f;

        do_in_committed_transaction(
          f.session(), [&] { return f.insert_truncate_entry(f.session(), 100, 300); });

        WT_TRUNCATE *entry = truncate_list_head(*f.layered_table());
        entry->start_ts = 30;
        entry->durable_ts = 30;

        WHEN(
          "the non-ingest conflict check is called with read_timestamp before the truncate "
          "timestamp")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(session_2, [&] {
                set_read_timestamp(session_2, 29);
                return f.detect_non_ingest_conflict(session_2, 200, 400);
            });

            THEN("it returns WT_ROLLBACK (truncate is not visible at this read timestamp)")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN(
          "the non-ingest conflict check is called with read_timestamp equal to the truncate "
          "timestamp")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(session_2, [&] {
                set_read_timestamp(session_2, 30);
                return f.detect_non_ingest_conflict(session_2, 200, 400);
            });

            THEN("it returns 0 (truncate is visible at this read timestamp)")
            {
                REQUIRE(result == 0);
            }
        }

        WHEN(
          "the non-ingest conflict check is called with read_timestamp after the truncate "
          "timestamp")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(session_2, [&] {
                set_read_timestamp(session_2, 31);
                return f.detect_non_ingest_conflict(session_2, 200, 400);
            });

            THEN("it returns 0 (truncate is visible at this read timestamp)")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO("non-ingest write conflict with a single-key uncommitted range",
  "[truncate_list][write_conflict]")
{
    GIVEN("a single-key uncommitted range [100, 100]")
    {
        write_conflict_fixture f;

        do_in_uncommitted_transaction(
          f.session(), [&] { return f.insert_truncate_entry(f.session(), 100, 100); });

        const auto cleanup = rollback_on_exit(f.session());

        WHEN("the new range exactly matches the existing range")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_non_ingest_conflict(session_2, 100, 100); });

            THEN("it returns WT_ROLLBACK")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the new range ends just before the existing range")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_non_ingest_conflict(session_2, 99, 99); });

            THEN("it returns 0 (no overlap)")
            {
                REQUIRE(result == 0);
            }
        }

        WHEN("the new range starts just after the existing range")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_non_ingest_conflict(session_2, 101, 101); });

            THEN("it returns 0 (no overlap)")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO("non-ingest write conflict with two non-overlapping uncommitted ranges",
  "[truncate_list][write_conflict]")
{
    GIVEN("uncommitted ranges [100, 200] and [400, 500]")
    {
        write_conflict_fixture f;

        do_in_uncommitted_transaction(f.session(), [&] {
            CHECK(f.insert_truncate_entry(f.session(), 100, 200) == 0);
            CHECK(f.insert_truncate_entry(f.session(), 400, 500) == 0);
            return 0;
        });

        const auto cleanup = rollback_on_exit(f.session());

        WHEN("the new range is inside the first existing range")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_non_ingest_conflict(session_2, 150, 175); });

            THEN("it returns WT_ROLLBACK")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the new range is inside the second existing range")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_non_ingest_conflict(session_2, 450, 475); });

            THEN("it returns WT_ROLLBACK")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the new range falls between the two existing ranges")
        {
            auto *session_2 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_2, [&] { return f.detect_non_ingest_conflict(session_2, 250, 350); });

            THEN("it returns 0 (no overlap)")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO("non-ingest write conflict with overlapping committed and uncommitted ranges",
  "[truncate_list][write_conflict]")
{
    GIVEN("a committed range [100, 300] and an uncommitted range [200, 400]")
    {
        write_conflict_fixture f;

        do_in_committed_transaction(
          f.session(), [&] { return f.insert_truncate_entry(f.session(), 100, 300); });

        auto *session_2 = f.create_session();

        do_in_uncommitted_transaction(
          session_2, [&] { return f.insert_truncate_entry(session_2, 200, 400); });

        const auto cleanup = rollback_on_exit(session_2);

        WHEN("the new range overlaps only the committed range")
        {
            auto *session_3 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_3, [&] { return f.detect_non_ingest_conflict(session_3, 50, 150); });

            THEN("it returns 0 (committed range is visible)")
            {
                REQUIRE(result == 0);
            }
        }

        WHEN("the new range overlaps the uncommitted range")
        {
            auto *session_3 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_3, [&] { return f.detect_non_ingest_conflict(session_3, 250, 350); });

            THEN("it returns WT_ROLLBACK (uncommitted range covers the overlap)")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the new range overlaps only the uncommitted range")
        {
            auto *session_3 = f.create_session();

            const auto result = do_in_rolled_back_transaction(
              session_3, [&] { return f.detect_non_ingest_conflict(session_3, 350, 450); });

            THEN("it returns WT_ROLLBACK")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }
    }
}
