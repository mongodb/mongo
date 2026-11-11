/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <atomic>
#include <catch2/catch.hpp>
#include <chrono>
#include <thread>

#include "wt_internal.h"
#include "../wrappers/connection_wrapper.h"

/*
 * A thread holding a read/write lock for reading can ask whether a writer is queued behind it, so
 * it can step aside. Check the answer with no writer, with a writer queued, and after the writer is
 * done.
 */

namespace {

WT_SESSION_IMPL *
open_session(WT_CONNECTION *conn)
{
    WT_SESSION *session;
    REQUIRE(conn->open_session(conn, nullptr, nullptr, &session) == 0);
    return ((WT_SESSION_IMPL *)session);
}

/* Wait up to ten seconds for a writer to queue on the lock. */
bool
wait_for_writer(WT_RWLOCK *l)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!__wt_rwlock_writer_waiting(l)) {
        if (std::chrono::steady_clock::now() > deadline)
            return (false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return (true);
}

} // namespace

TEST_CASE("Read/write lock reports a writer queued behind its readers", "[rwlock]")
{
    connection_wrapper conn_wrapper(".", "create");
    WT_CONNECTION *conn = conn_wrapper.get_wt_connection();
    WT_SESSION_IMPL *reader = open_session(conn);
    WT_SESSION_IMPL *other_reader = open_session(conn);
    WT_SESSION_IMPL *writer = open_session(conn);
    WT_RWLOCK lock;

    REQUIRE(__wt_rwlock_init(reader, &lock) == 0);

    /* Nobody holds the lock. */
    CHECK_FALSE(__wt_rwlock_writer_waiting(&lock));

    /* Readers alone do not look like a writer. */
    __wt_readlock(reader, &lock);
    __wt_readlock(other_reader, &lock);
    CHECK_FALSE(__wt_rwlock_writer_waiting(&lock));
    __wt_readunlock(other_reader, &lock);
    CHECK_FALSE(__wt_rwlock_writer_waiting(&lock));

    /* A writer blocks behind the remaining reader. */
    std::atomic<bool> writer_acquired{false};
    std::thread writer_thread([&] {
        __wt_writelock(writer, &lock);
        writer_acquired = true;
        __wt_writeunlock(writer, &lock);
    });
    bool queued = wait_for_writer(&lock);
    bool acquired_early = writer_acquired;

    /* Let the writer through before any check can end the test with the writer still blocked. */
    __wt_readunlock(reader, &lock);
    writer_thread.join();

    CHECK(queued);
    CHECK_FALSE(acquired_early);
    CHECK(writer_acquired);

    /* The writer is done, nothing is queued. */
    CHECK_FALSE(__wt_rwlock_writer_waiting(&lock));

    __wt_rwlock_destroy(reader, &lock);
}
