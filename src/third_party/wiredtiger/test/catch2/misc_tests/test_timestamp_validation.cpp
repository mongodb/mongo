/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include <string>
#include "../wrappers/mock_session.h"

static constexpr wt_timestamp_t stable_timestamp = 5;
static constexpr wt_timestamp_t oldest_timestamp = 3;
static constexpr wt_timestamp_t checkpoint_timestamp = 7;
static constexpr wt_timestamp_t checkpoint_oldest_timestamp = 4;

static std::shared_ptr<mock_session>
setup_session()
{
    auto session_mock = mock_session::build_test_mock_session();
    session_mock->setup_block_manager_file_operations();
    WT_SESSION_IMPL *session = session_mock->get_wt_session_impl();
    WT_TXN_GLOBAL *txn_global = &S2C(session)->txn_global;

    __wt_atomic_store_uint64_relaxed(&txn_global->stable_timestamp, stable_timestamp);
    __wt_atomic_store_bool_release(&txn_global->has_stable_timestamp, true);
    __wt_atomic_store_uint64_relaxed(&txn_global->oldest_timestamp, oldest_timestamp);
    __wt_atomic_store_bool_release(&txn_global->has_oldest_timestamp, true);
    S2BT(session)->checkpoint_timestamp = checkpoint_timestamp;
    __wt_atomic_store_uint64_release(
      &S2C(session)->disaggregated_storage.last_checkpoint_oldest_timestamp,
      checkpoint_oldest_timestamp);
    return session_mock;
}

/* Delta states a caller can pass: known not from a delta, known from a delta, and unknown. */
static const struct {
    bool known;
    bool from_delta;
    const char *expected;
} delta_states[] = {{true, false, "false"}, {true, true, "true"}, {false, false, nullptr}};

/*
 * Check the message for the failure reason and timestamp context. A null from_delta means the delta
 * state was unknown and must be left out of the message.
 */
static void
check_message(const std::string &message, const std::string &reason, const char *from_delta,
  wt_timestamp_t expected_stable = stable_timestamp,
  wt_timestamp_t expected_oldest = oldest_timestamp,
  wt_timestamp_t expected_checkpoint = checkpoint_timestamp,
  wt_timestamp_t expected_checkpoint_oldest = checkpoint_oldest_timestamp)
{
    char buf[WT_TS_INT_STRING_SIZE];

    INFO(message);
    /* Confirm the intended failure before checking its timestamp context. */
    REQUIRE(message.find(reason) != std::string::npos);
    CHECK(message.find(std::string("connection stable timestamp ") +
            __wt_timestamp_to_string(expected_stable, buf)) != std::string::npos);
    CHECK(message.find(std::string("connection oldest timestamp ") +
            __wt_timestamp_to_string(expected_oldest, buf)) != std::string::npos);
    CHECK(message.find(std::string("checkpoint stable timestamp ") +
            __wt_timestamp_to_string(expected_checkpoint, buf)) != std::string::npos);
    CHECK(message.find(std::string("checkpoint oldest timestamp ") +
            __wt_timestamp_to_string(expected_checkpoint_oldest, buf)) != std::string::npos);
    if (from_delta == nullptr)
        CHECK(message.find("page built from delta") == std::string::npos);
    else
        CHECK(
          message.find(std::string("page built from delta: ") + from_delta) != std::string::npos);
}

static void
check_aggregate_failure(WT_TIME_AGGREGATE *ta, WT_TIME_AGGREGATE *parent, const std::string &reason)
{
    for (const auto &state : delta_states) {
        CAPTURE(state.known, state.from_delta);
        auto session_mock = setup_session();
        WT_SESSION_IMPL *session = session_mock->get_wt_session_impl();
        REQUIRE(__wt_time_aggregate_validate(
                  session, ta, parent, state.known, state.from_delta, false) == EINVAL);
        check_message(
          session_mock->get_last_message(), "aggregate time window " + reason, state.expected);
    }
}

static void
check_value_failure(WT_TIME_WINDOW *tw, WT_TIME_AGGREGATE *parent, const std::string &reason)
{
    for (const auto &state : delta_states) {
        CAPTURE(state.known, state.from_delta);
        auto session_mock = setup_session();
        WT_SESSION_IMPL *session = session_mock->get_wt_session_impl();
        /* Missing prepared IDs are rejected only when prepared information is preserved. */
        F_SET(S2C(session), WT_CONN_PRESERVE_PREPARED);
        REQUIRE(__wt_time_value_validate(
                  session, tw, parent, state.known, state.from_delta, false) == EINVAL);
        check_message(session_mock->get_last_message(), reason, state.expected);
    }
}

TEST_CASE(
  "Validation errors report live timestamps before the error callback advances them", "[timestamp]")
{
    auto session_mock = setup_session();
    WT_SESSION_IMPL *session = session_mock->get_wt_session_impl();
    F_SET(session, WT_SESSION_SAVE_ERRORS);
    session->event_handler->handle_error = [](WT_EVENT_HANDLER *handler, WT_SESSION *wt_session,
                                             int error, const char *message) {
        int ret = handle_wiredtiger_error(handler, wt_session, error, message);
        WT_SESSION_IMPL *session_impl = reinterpret_cast<WT_SESSION_IMPL *>(wt_session);
        WT_TXN_GLOBAL *txn_global = &S2C(session_impl)->txn_global;

        /* Advance timestamps between callback logging and session error storage. */
        __wt_atomic_store_uint64_relaxed(&txn_global->stable_timestamp, 100);
        __wt_atomic_store_uint64_relaxed(&txn_global->oldest_timestamp, 90);
        S2BT(session_impl)->checkpoint_timestamp = 80;
        __wt_atomic_store_uint64_release(
          &S2C(session_impl)->disaggregated_storage.last_checkpoint_oldest_timestamp, 70);
        return ret;
    };

    bool aggregate = false, from_delta = false;
    SECTION("Aggregate")
    {
        aggregate = true;
    }
    SECTION("Value") {}
    SECTION("Delta value")
    {
        from_delta = true;
    }

    WT_TIME_AGGREGATE parent;
    WT_TIME_AGGREGATE_INIT(&parent);
    std::string reason;
    if (aggregate) {
        WT_TIME_AGGREGATE ta;
        WT_TIME_AGGREGATE_INIT(&ta);
        ta.newest_start_durable_ts = checkpoint_timestamp + 1;
        reason = "has a newest start durable time after the stable point";
        REQUIRE(
          __wt_time_aggregate_validate(session, &ta, &parent, true, from_delta, false) == EINVAL);
    } else {
        WT_TIME_WINDOW tw;
        WT_TIME_WINDOW_INIT(&tw);
        tw.durable_start_ts = checkpoint_timestamp + 1;
        reason = "has a durable start time after the stable point";
        REQUIRE(__wt_time_value_validate(session, &tw, &parent, true, from_delta, false) == EINVAL);
    }

    REQUIRE(__wt_get_stable_timestamp(session) == 100);
    const char *expected_from_delta = from_delta ? "true" : "false";
    check_message(session_mock->get_last_message(), reason, expected_from_delta);
    REQUIRE(session->err_info.err_msg != nullptr);
    check_message(session->err_info.err_msg, reason, expected_from_delta);
}

TEST_CASE("Validation errors report unset connection timestamps", "[timestamp]")
{
    auto session_mock = setup_session();
    WT_SESSION_IMPL *session = session_mock->get_wt_session_impl();
    WT_TXN_GLOBAL *txn_global = &S2C(session)->txn_global;
    wt_timestamp_t expected_stable = stable_timestamp;
    wt_timestamp_t expected_oldest = oldest_timestamp;

    SECTION("Oldest unset")
    {
        __wt_atomic_store_bool_release(&txn_global->has_oldest_timestamp, false);
        expected_oldest = WT_TS_NONE;
    }
    SECTION("Stable unset")
    {
        __wt_atomic_store_bool_release(&txn_global->has_stable_timestamp, false);
        expected_stable = WT_TS_NONE;
    }
    SECTION("Stable unset with recovery timestamp")
    {
        __wt_atomic_store_bool_release(&txn_global->has_stable_timestamp, false);
        txn_global->recovery_timestamp = 2;
        expected_stable = txn_global->recovery_timestamp;
    }

    WT_TIME_AGGREGATE ta;
    WT_TIME_AGGREGATE_INIT(&ta);
    ta.oldest_start_ts = 20;
    ta.newest_stop_ts = 10;
    REQUIRE(__wt_time_aggregate_validate(session, &ta, nullptr, false, false, false) == EINVAL);
    check_message(session_mock->get_last_message(),
      "has an oldest start time after its newest stop time", nullptr, expected_stable,
      expected_oldest);
}

TEST_CASE("Aggregate validation handles unset checkpoint timestamps", "[timestamp]")
{
    auto session_mock = setup_session();
    WT_SESSION_IMPL *session = session_mock->get_wt_session_impl();
    wt_timestamp_t expected_checkpoint = checkpoint_timestamp;
    wt_timestamp_t expected_checkpoint_oldest = checkpoint_oldest_timestamp;

    SECTION("No data handle")
    {
        __wt_free(session, session->dhandle->handle);
        __wt_free(session, session->dhandle);
        expected_checkpoint = WT_TS_NONE;
    }
    SECTION("Checkpoint timestamps unset")
    {
        S2BT(session)->checkpoint_timestamp = WT_TS_NONE;
        expected_checkpoint = WT_TS_NONE;
        __wt_atomic_store_uint64_release(
          &S2C(session)->disaggregated_storage.last_checkpoint_oldest_timestamp, WT_TS_NONE);
        expected_checkpoint_oldest = WT_TS_NONE;
    }

    WT_TIME_AGGREGATE ta;
    WT_TIME_AGGREGATE_INIT(&ta);
    REQUIRE(__wt_time_aggregate_validate(session, &ta, nullptr, false, false, false) == 0);

    ta.oldest_start_ts = 20;
    ta.newest_stop_ts = 10;
    REQUIRE(__wt_time_aggregate_validate(session, &ta, nullptr, false, false, true) == EINVAL);
    REQUIRE(__wt_time_aggregate_validate(session, &ta, nullptr, false, false, false) == EINVAL);
    check_message(session_mock->get_last_message(),
      "has an oldest start time after its newest stop time", nullptr, stable_timestamp,
      oldest_timestamp, expected_checkpoint, expected_checkpoint_oldest);
}

TEST_CASE("Aggregate validation reports internal consistency errors", "[timestamp]")
{
    WT_TIME_AGGREGATE ta;
    WT_TIME_AGGREGATE_INIT(&ta);
    std::string reason;

    SECTION("Oldest start after newest stop")
    {
        ta.oldest_start_ts = 20;
        ta.newest_start_durable_ts = 20;
        ta.newest_stop_ts = 10;
        reason = "has an oldest start time after its newest stop time";
    }
    SECTION("Newest transaction after newest stop transaction")
    {
        ta.newest_txn = 20;
        ta.newest_stop_txn = 10;
        reason = "has a newest transaction after its newest stop transaction";
    }
    SECTION("Oldest start after newest durable start")
    {
        ta.oldest_start_ts = 20;
        ta.newest_start_durable_ts = 10;
        reason = "has an oldest start time after its newest start durable time";
    }
    SECTION("Newest stop after newest durable stop")
    {
        ta.newest_stop_ts = 20;
        ta.newest_stop_durable_ts = 10;
        reason = "has a newest stop time after its newest stop durable time";
    }
    SECTION("Newest durable start after newest durable stop")
    {
        ta.newest_start_durable_ts = 30;
        ta.newest_stop_ts = 10;
        ta.newest_stop_durable_ts = 20;
        reason = "has a newest start durable time after its newest stop durable time";
    }
    SECTION("Newest durable stop before oldest start")
    {
        ta.oldest_start_ts = 20;
        ta.newest_start_durable_ts = 20;
        ta.newest_stop_durable_ts = 10;
        reason = "has a newest stop durable time before its oldest start time";
    }
    check_aggregate_failure(&ta, nullptr, reason);
}

TEST_CASE("Aggregate validation reports parent containment errors", "[timestamp]")
{
    WT_TIME_AGGREGATE ta, parent;
    WT_TIME_AGGREGATE_INIT(&ta);
    ta.oldest_start_ts = 10;
    ta.newest_start_durable_ts = 20;
    ta.newest_stop_ts = 30;
    ta.newest_stop_durable_ts = 40;
    ta.newest_txn = 10;
    ta.newest_stop_txn = 20;
    WT_TIME_AGGREGATE_COPY(&parent, &ta);
    std::string reason;

    SECTION("Newest durable start after parent")
    {
        parent.newest_start_durable_ts = 19;
        reason = "has a newest start durable time after its parent's";
    }
    SECTION("Newest durable stop after parent")
    {
        parent.newest_stop_durable_ts = 39;
        reason = "has a newest stop durable time after its parent's";
    }
    SECTION("Oldest start before parent")
    {
        parent.oldest_start_ts = 11;
        reason = "has an oldest start time before its parent's";
    }
    SECTION("Newest transaction after parent")
    {
        parent.newest_txn = 9;
        reason = "has a newest transaction after its parent's";
    }
    SECTION("Newest stop after parent")
    {
        parent.newest_stop_ts = 29;
        reason = "has a newest stop time after its parent's";
    }
    SECTION("Newest stop transaction after parent")
    {
        parent.newest_stop_txn = 19;
        reason = "has a newest stop transaction after its parent's";
    }
    SECTION("Prepared aggregate with unprepared parent")
    {
        ta.prepare = 1;
        reason = "is prepared but its parent is not";
    }
    check_aggregate_failure(&ta, &parent, reason);
}

TEST_CASE("Aggregate validation reports stable point errors", "[timestamp]")
{
    WT_TIME_AGGREGATE ta, parent;
    WT_TIME_AGGREGATE_INIT(&ta);
    WT_TIME_AGGREGATE_INIT(&parent);
    std::string reason;

    SECTION("Newest durable start after stable")
    {
        ta.newest_start_durable_ts = stable_timestamp + 1;
        reason = "has a newest start durable time after";
    }
    SECTION("Newest durable stop after stable")
    {
        ta.newest_stop_durable_ts = stable_timestamp + 1;
        reason = "has a newest stop durable time after";
    }
    check_aggregate_failure(
      &ta, &parent, reason + " the stable point with an empty parent aggregate time window");
}

TEST_CASE("Value validation reports timestamp and transaction ordering errors", "[timestamp]")
{
    WT_TIME_WINDOW tw;
    WT_TIME_WINDOW_INIT(&tw);
    std::string reason;

    SECTION("Start after stop")
    {
        tw.start_ts = 20;
        tw.durable_start_ts = 20;
        tw.stop_ts = 10;
        tw.durable_stop_ts = 30;
        reason = "has a start time after its stop time";
    }
    SECTION("Start transaction after stop transaction")
    {
        tw.start_txn = 20;
        tw.stop_txn = 10;
        tw.stop_ts = WT_TS_NONE;
        reason = "has a start transaction after its stop transaction";
    }
    SECTION("Start after durable start")
    {
        tw.start_ts = 20;
        tw.durable_start_ts = 10;
        reason = "has a start time after its durable start time";
    }
    SECTION("Stop after durable stop")
    {
        tw.stop_ts = 20;
        tw.durable_stop_ts = 10;
        reason = "has a stop time after its durable stop time";
    }
    SECTION("Durable start after stop")
    {
        tw.durable_start_ts = 20;
        tw.stop_ts = 10;
        tw.durable_stop_ts = 30;
        reason = "has a durable start time after its stop time";
    }
    SECTION("Durable start after durable stop")
    {
        tw.durable_start_ts = 20;
        tw.durable_stop_ts = 10;
        reason = "has a durable start time after its durable stop time";
    }
    check_value_failure(&tw, nullptr, "value time window " + reason);
}

TEST_CASE("Value validation reports prepared start errors", "[timestamp]")
{
    WT_TIME_WINDOW tw;
    WT_TIME_WINDOW_INIT(&tw);
    tw.start_prepare_ts = 10;
    tw.start_prepared_id = 1;
    std::string reason;

    SECTION("Missing prepare timestamp")
    {
        tw.start_prepare_ts = WT_TS_NONE;
        reason = "has no start prepare time";
    }
    SECTION("Missing prepared ID")
    {
        tw.start_prepared_id = WT_PREPARED_ID_NONE;
        reason = "has no start prepared id";
    }
    SECTION("Commit timestamp set")
    {
        tw.start_ts = 5;
        tw.durable_start_ts = 5;
        reason = "has a start time set";
    }
    SECTION("Durable timestamp set")
    {
        tw.durable_start_ts = 5;
        reason = "has a durable start time set";
    }
    check_value_failure(&tw, nullptr, "Start prepared value time window " + reason);
}

TEST_CASE("Value validation reports prepared stop errors", "[timestamp]")
{
    WT_TIME_WINDOW tw;
    WT_TIME_WINDOW_INIT(&tw);
    tw.stop_prepare_ts = 10;
    tw.stop_prepared_id = 1;
    std::string reason;

    SECTION("Missing prepare timestamp")
    {
        tw.stop_prepare_ts = WT_TS_NONE;
        reason = "has no stop prepare time";
    }
    SECTION("Missing prepared ID")
    {
        tw.stop_prepared_id = WT_PREPARED_ID_NONE;
        reason = "has no stop prepared id";
    }
    SECTION("Commit timestamp set")
    {
        tw.stop_ts = 5;
        tw.durable_stop_ts = 5;
        reason = "has a stop time set";
    }
    SECTION("Durable timestamp set")
    {
        tw.durable_stop_ts = 5;
        reason = "has a durable stop time set";
    }
    check_value_failure(&tw, nullptr, "Stop prepared value time window " + reason);
}

TEST_CASE("Value validation reports incomplete stop information", "[timestamp]")
{
    WT_TIME_WINDOW tw;
    WT_TIME_WINDOW_INIT(&tw);
    tw.stop_txn = 10;
    check_value_failure(
      &tw, nullptr, "Time window has a stop transaction id but no prepare or stop time set");
}

TEST_CASE("Value validation reports parent containment errors", "[timestamp]")
{
    WT_TIME_WINDOW tw;
    WT_TIME_WINDOW_INIT(&tw);
    tw.start_ts = 10;
    tw.durable_start_ts = 20;
    tw.stop_ts = 30;
    tw.durable_stop_ts = 40;
    tw.start_txn = 10;
    tw.stop_txn = 20;
    WT_TIME_AGGREGATE parent;
    WT_TIME_AGGREGATE_INIT(&parent);
    parent.oldest_start_ts = 10;
    parent.newest_start_durable_ts = 20;
    parent.newest_stop_ts = 30;
    parent.newest_stop_durable_ts = 40;
    parent.newest_txn = 10;
    parent.newest_stop_txn = 20;
    std::string reason;

    SECTION("Durable start after parent")
    {
        parent.newest_start_durable_ts = 19;
        reason = "has a durable start time after its parent's newest durable start time";
    }
    SECTION("Start prepare before parent")
    {
        tw.start_ts = WT_TS_NONE;
        tw.durable_start_ts = WT_TS_NONE;
        tw.start_prepare_ts = 9;
        tw.start_prepared_id = 1;
        parent.prepare = 1;
        reason = "has a start prepare time before its parent's oldest start time";
    }
    SECTION("Start before parent")
    {
        parent.oldest_start_ts = 11;
        reason = "has a start time before its parent's oldest start time";
    }
    SECTION("Start transaction after parent")
    {
        parent.newest_txn = 9;
        reason = "has a start transaction after its parent's newest transaction";
    }
    SECTION("Durable stop after parent")
    {
        parent.newest_stop_durable_ts = 39;
        reason = "has a durable stop time after its parent's newest durable stop time";
    }
    SECTION("Stop prepare after parent")
    {
        tw.stop_ts = WT_TS_MAX;
        tw.durable_stop_ts = WT_TS_NONE;
        tw.stop_prepare_ts = 31;
        tw.stop_prepared_id = 1;
        parent.prepare = 1;
        reason = "has a stop prepare time after its parent's newest stop time";
    }
    SECTION("Stop after parent")
    {
        parent.newest_stop_ts = 29;
        reason = "has a stop time after its parent's newest stop time";
    }
    SECTION("Stop transaction after parent")
    {
        parent.newest_stop_txn = 19;
        reason = "has a stop transaction after its parent's newest stop transaction";
    }
    SECTION("Prepared value with unprepared parent")
    {
        tw.start_ts = WT_TS_NONE;
        tw.durable_start_ts = WT_TS_NONE;
        tw.start_prepare_ts = 10;
        tw.start_prepared_id = 1;
        reason = "is prepared but its parent is not";
    }
    check_value_failure(&tw, &parent, "value time window " + reason);
}

TEST_CASE("Value validation reports stable point errors", "[timestamp]")
{
    WT_TIME_WINDOW tw;
    WT_TIME_WINDOW_INIT(&tw);
    WT_TIME_AGGREGATE parent;
    WT_TIME_AGGREGATE_INIT(&parent);
    std::string reason;

    SECTION("Durable start after stable")
    {
        tw.durable_start_ts = checkpoint_timestamp + 1;
        reason = "has a durable start time after";
    }
    SECTION("Durable stop after stable")
    {
        tw.durable_stop_ts = checkpoint_timestamp + 1;
        reason = "has a durable stop time after";
    }
    check_value_failure(&tw, &parent,
      "time window " + reason + " the stable point with an empty parent aggregate time window");
}
