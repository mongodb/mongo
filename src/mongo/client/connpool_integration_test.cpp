/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
    ASSERT_THROWS(pool.get(host, 1), AssertionException);

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

}  // namespace

}  // namespace mongo
