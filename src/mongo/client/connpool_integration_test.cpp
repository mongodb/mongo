// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// IWYU pragma: no_include "cxxabi.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/connpool.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mongo {
namespace {

TEST(ConnectionPoolTest, ConnectionPoolMaxInUseConnectionsTest) {
    DBConnectionPool pool;

    auto fixture = unittest::getFixtureConnectionString();
    auto host = fixture.getServers()[0].toString();

    stdx::condition_variable cv;
    std::mutex mutex;
    int counter = 0;

    pool.setMaxInUse(2);

    // Check out maxInUse connections.
    auto conn1 = pool.get(host);
    ASSERT(conn1);
    auto conn2 = pool.get(host);
    ASSERT(conn2);

    // Try creating a new one, should block until we release one.
    stdx::thread t([&] {
        {
            std::lock_guard<std::mutex> lk(mutex);
            counter++;
        }

        cv.notify_one();

        auto conn3 = pool.get(host);

        {
            std::lock_guard<std::mutex> lk(mutex);
            counter++;
        }

        cv.notify_one();
        pool.release(host, conn3);
    });

    // First thread should be blocked.
    {
        std::unique_lock<std::mutex> lk(mutex);
        cv.wait(lk, [&] { return counter == 1; });
    }

    // Return one to the pool, thread should be un-blocked.
    pool.release(host, conn2);

    {
        std::unique_lock<std::mutex> lk(mutex);
        cv.wait(lk, [&] { return counter == 2; });
    }

    t.join();

    pool.release(host, conn1);
}

TEST(ConnectionPoolTest, ConnectionPoolMaxInUseTimeoutTest) {
    DBConnectionPool pool;

    auto fixture = unittest::getFixtureConnectionString();
    auto host = fixture.getServers()[0].toString();

    pool.setMaxInUse(2);

    // Check out maxInUse connections.
    auto conn1 = pool.get(host, 1);
    ASSERT(conn1);
    auto conn2 = pool.get(host, 1);
    ASSERT(conn2);

    // Try creating a new connection with a 1-second timeout, should block,
    // then should time out.
    ASSERT_THROWS(pool.get(host, 1), AssertionException);

    pool.release(host, conn1);
    pool.release(host, conn2);
}

TEST(ConnectionPoolTest, ConnectionPoolShutdownLogicTest) {
    DBConnectionPool pool;

    auto fixture = unittest::getFixtureConnectionString();
    auto host = fixture.getServers()[0].toString();

    stdx::condition_variable cv;
    std::mutex mutex;
    int counter = 0;

    pool.setMaxInUse(2);

    // Check out maxInUse connections.
    auto conn1 = pool.get(host);
    ASSERT(conn1);
    auto conn2 = pool.get(host);
    ASSERT(conn2);

    // Attempt to open a new connection, should block.
    stdx::thread t([&] {
        {
            std::lock_guard<std::mutex> lk(mutex);
            counter++;
        }

        cv.notify_one();

        ASSERT_THROWS(pool.get(host), AssertionException);

        {
            std::lock_guard<std::mutex> lk(mutex);
            counter++;
        }

        cv.notify_one();
    });

    // Wait for new thread to block.
    {
        std::unique_lock<std::mutex> lk(mutex);
        cv.wait(lk, [&] { return counter == 1; });
    }

    // Shut down the pool, this should unblock our waiting connection.
    pool.shutdown();
    {
        std::unique_lock<std::mutex> lk(mutex);
        cv.wait(lk, [&] { return counter == 2; });
    }

    // Attempt to open a new connection, should fail.
    ASSERT_THROWS(pool.get(host), AssertionException);

    t.join();

    pool.release(host, conn1);
    pool.release(host, conn2);
}

}  // namespace

}  // namespace mongo
