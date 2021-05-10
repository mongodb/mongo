
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/connpool.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ConnectionPoolTest, ConnectionPoolMaxInUseConnectionsTest) {
    DBConnectionPool pool;

    auto fixture = unittest::getFixtureConnectionString();
    auto host = fixture.getServers()[0].toString();

    stdx::condition_variable cv;
    stdx::mutex mutex;
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
            stdx::lock_guard<stdx::mutex> lk(mutex);
            counter++;
        }

        cv.notify_one();

        auto conn3 = pool.get(host);

        {
            stdx::lock_guard<stdx::mutex> lk(mutex);
            counter++;
        }

        cv.notify_one();
        pool.release(host, conn3);
    });

    // First thread should be blocked.
    {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        cv.wait(lk, [&] { return counter == 1; });
        // We expect this wait to time out as the condition is never true.
        auto ret = cv.wait_for(lk, stdx::chrono::seconds{1}, [&] { return counter == 2; });
        ASSERT_FALSE(ret) << "Thread is expected to be blocked";
    }

    // Return one to the pool, thread should be un-blocked.
    pool.release(host, conn2);

    {
        stdx::unique_lock<stdx::mutex> lk(mutex);
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
    Timer t;
    ASSERT_THROWS(pool.get(host, 1), AssertionException);
    // The timeout should be respected, throws after 1 second.
    ASSERT_GT(t.millis(), 800);

    pool.release(host, conn1);
    pool.release(host, conn2);
}

TEST(ConnectionPoolTest, ConnectionPoolShutdownLogicTest) {
    DBConnectionPool pool;

    auto fixture = unittest::getFixtureConnectionString();
    auto host = fixture.getServers()[0].toString();

    stdx::condition_variable cv;
    stdx::mutex mutex;
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
            stdx::lock_guard<stdx::mutex> lk(mutex);
            counter++;
        }

        cv.notify_one();

        ASSERT_THROWS(pool.get(host), AssertionException);

        {
            stdx::lock_guard<stdx::mutex> lk(mutex);
            counter++;
        }

        cv.notify_one();
    });

    // Wait for new thread to block.
    {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        cv.wait(lk, [&] { return counter == 1; });
    }

    // Shut down the pool, this should unblock our waiting connection.
    pool.shutdown();
    {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        cv.wait(lk, [&] { return counter == 2; });
    }

    // Attempt to open a new connection, should fail.
    ASSERT_THROWS(pool.get(host), AssertionException);

    t.join();

    pool.release(host, conn1);
    pool.release(host, conn2);
}

#ifndef _WIN32

// Tests that internal condition variable is properly notified.
TEST(ConnectionPoolTest, ConnectionPoolConditionNotifyTest) {
    DBConnectionPool pool;

    auto fixture = unittest::getFixtureConnectionString();
    auto host = fixture.getServers()[0].toString();

    pool.setMaxInUse(1);

    // Check out maxInUse connections.
    auto conn1 = pool.get(host, 100);
    ASSERT(conn1);

    stdx::condition_variable cv;
    stdx::mutex mutex;
    bool aboutToGetConnection = false;

    // Release the conn1 asynchronously, expect the internal condition to be notified.
    stdx::thread t([&] {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        cv.wait(lk, [&] { return aboutToGetConnection; });
        lk.release();
        // Small chance of race is intentional, if the conn1 is released before
        // conn2 is requested, the test is still valid.
        stdx::this_thread::sleep_for(stdx::chrono::milliseconds(10));
        std::cerr << "Releasing first connection..." << std::endl;
        pool.decrementEgress(host, conn1);
        delete conn1;
    });

    {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        aboutToGetConnection = true;
        cv.notify_all();
    }

    auto conn2 = pool.get(host, 100);
    ASSERT(conn2);
    t.join();

    pool.release(host, conn2);
}

#endif

}  // namespace

}  // namespace mongo
