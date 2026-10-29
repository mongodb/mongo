/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "wt_internal.h"
#include "../wrappers/connection_wrapper.h"

/*
 * Verify that connection API messages identify the active method during nested and concurrent
 * calls, and that returning from a call restores the previous message label.
 */

namespace {

struct message_log {
    std::mutex lock;
    std::vector<std::string> messages;
    /* Called for every message after it is recorded; it may call connection methods. */
    std::function<void(WT_SESSION *, const std::string &)> on_message;
};

struct event_handler : WT_EVENT_HANDLER {
    message_log *log;
};

extern "C" int
record_message(WT_EVENT_HANDLER *handler, WT_SESSION *session, const char *message)
{
    message_log *log = static_cast<event_handler *>(handler)->log;
    {
        std::lock_guard<std::mutex> guard(log->lock);
        log->messages.emplace_back(message);
    }
    if (log->on_message)
        log->on_message(session, message);
    return (0);
}

/* The session name an API message is labeled with: the text between ", " and ": [WT_VERB_API]". */
std::string
message_label(const std::string &message)
{
    const std::size_t category = message.find(": [WT_VERB_API]");
    if (category == std::string::npos)
        return ("");
    const std::size_t start = message.rfind(", ", category);
    return (start == std::string::npos ? "" : message.substr(start + 2, category - start - 2));
}

/* The connection method a "CALL:" message announces, or an empty string. */
std::string
called_method(const std::string &message)
{
    const std::string call = "CALL: WT_CONNECTION:";
    const std::size_t start = message.find(call);
    if (start == std::string::npos)
        return ("");
    const std::size_t end = message.find_first_of(" \n", start + call.size());
    return (message.substr(
      start + call.size(), end == std::string::npos ? end : end - start - call.size()));
}

/* The label of the first recorded message containing the text. */
std::string
label_of(message_log &log, const std::string &text)
{
    std::lock_guard<std::mutex> guard(log.lock);
    for (const auto &message : log.messages)
        if (message.find(text) != std::string::npos)
            return (message_label(message));
    return ("<no such message>");
}

} // namespace

TEST_CASE("Connection API: messages are labeled with the connection method", "[conn_api_name]")
{
    message_log log;
    event_handler wrap = {};
    wrap.handle_message = record_message;
    wrap.log = &log;

    connection_wrapper conn("WT_TEST.conn_api_name_label", "create,verbose=[api:1]", &wrap);
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_SESSION_IMPL *default_session = conn.get_wt_connection_impl()->default_session;

    char ts[WT_TS_HEX_STRING_SIZE];
    (void)wt_conn->query_timestamp(wt_conn, ts, "get=all_durable");
    REQUIRE(
      label_of(log, "CALL: WT_CONNECTION:query_timestamp") == "WT_CONNECTION.query_timestamp");

    /* Once the method returns, the default session reports its own name again. */
    __wt_verbose(default_session, WT_VERB_API, "%s", "outside any connection method");
    REQUIRE(label_of(log, "outside any connection method") == default_session->name);
}

TEST_CASE(
  "Connection API: a nested connection call restores the outer method's name", "[conn_api_name]")
{
    message_log log;
    event_handler wrap = {};
    wrap.handle_message = record_message;
    wrap.log = &log;

    connection_wrapper conn("WT_TEST.conn_api_name_nested", "create,verbose=[api:1]", &wrap);
    WT_CONNECTION *wt_conn = conn.get_wt_connection();

    /*
     * An event handler can call connection methods while the connection method that wrote the
     * message is still running: nest one call inside another, then write a message from the outer
     * call.
     */
    bool nested = false;
    log.on_message = [&](WT_SESSION *session, const std::string &message) {
        if (nested || called_method(message) != "debug_info")
            return;
        nested = true;
        char ts[WT_TS_HEX_STRING_SIZE];
        (void)wt_conn->query_timestamp(wt_conn, ts, "get=all_durable");
        __wt_verbose((WT_SESSION_IMPL *)session, WT_VERB_API, "%s", "after the nested call");
    };
    REQUIRE(wt_conn->debug_info(wt_conn, "") == 0);
    log.on_message = nullptr;

    REQUIRE(nested);
    REQUIRE(
      label_of(log, "CALL: WT_CONNECTION:query_timestamp") == "WT_CONNECTION.query_timestamp");
    REQUIRE(label_of(log, "after the nested call") == "WT_CONNECTION.debug_info");
}

TEST_CASE("Connection API: concurrent connection calls label their own messages", "[conn_api_name]")
{
    const int iterations = 1000;

    message_log log;
    event_handler wrap = {};
    wrap.handle_message = record_message;
    wrap.log = &log;

    connection_wrapper conn("WT_TEST.conn_api_name_concurrent", "create,verbose=[api:1]", &wrap);
    WT_CONNECTION *wt_conn = conn.get_wt_connection();

    /* Each thread calls a different connection method, so a message's method tells its thread. */
    std::vector<std::thread> threads;
    threads.emplace_back([&] {
        char ts[WT_TS_HEX_STRING_SIZE];
        for (int i = 0; i < iterations; ++i)
            (void)wt_conn->query_timestamp(wt_conn, ts, "get=all_durable");
    });
    threads.emplace_back([&] {
        for (int i = 1; i <= iterations; ++i) {
            const std::string config =
              "oldest_timestamp=" + std::to_string(i) + ",stable_timestamp=" + std::to_string(i);
            (void)wt_conn->set_timestamp(wt_conn, config.c_str());
        }
    });
    threads.emplace_back([&] {
        for (int i = 0; i < iterations; ++i) {
            WT_SESSION *session;
            if (wt_conn->open_session(wt_conn, nullptr, nullptr, &session) == 0)
                (void)session->close(session, nullptr);
        }
    });
    for (auto &thread : threads)
        thread.join();

    /* Check messages. */
    std::lock_guard<std::mutex> guard(log.lock);
    std::size_t checked = 0;
    for (const auto &message : log.messages) {
        const std::string method = called_method(message);
        if (method.empty())
            continue;
        INFO(message);
        CHECK(message_label(message) == "WT_CONNECTION." + method);
        ++checked;
    }
    REQUIRE(checked >= 3 * iterations);
}
