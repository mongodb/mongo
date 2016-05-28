/** *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/executor/connection_pool_test_fixture.h"

#include "mongo/executor/connection_pool.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace executor {
namespace connection_pool_test_details {

class ConnectionPoolTest : public unittest::Test {
public:
protected:
    void setUp() override {}

    void tearDown() override {
        ConnectionImpl::clear();
        TimerImpl::clear();
    }

    void doneWith(const ConnectionPool::ConnectionHandle& swConn) {
        static_cast<ConnectionImpl*>(swConn.get())->indicateSuccess();
    }

private:
};

#define CONN2ID(swConn)                                                     \
    [](StatusWith<ConnectionPool::ConnectionHandle>& swConn) {              \
        ASSERT(swConn.isOK());                                              \
        return static_cast<ConnectionImpl*>(swConn.getValue().get())->id(); \
    }(swConn)

/**
 * Verify that we get the same connection if we grab one, return it and grab
 * another.
 */
TEST_F(ConnectionPoolTest, SameConn) {
    ConnectionPool pool(stdx::make_unique<PoolImpl>());

    // Grab and stash an id for the first request
    size_t conn1Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 conn1Id = CONN2ID(swConn);
                 doneWith(swConn.getValue());
             });

    // Grab and stash an id for the second request
    size_t conn2Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 conn2Id = CONN2ID(swConn);
                 doneWith(swConn.getValue());
             });

    // Verify that we hit them, and that they're the same
    ASSERT(conn1Id);
    ASSERT(conn2Id);
    ASSERT_EQ(conn1Id, conn2Id);
}

/**
 * Verify that a failed connection isn't returned to the pool
 */
TEST_F(ConnectionPoolTest, FailedConnDifferentConn) {
    ConnectionPool pool(stdx::make_unique<PoolImpl>());

    // Grab the first connection and indicate that it failed
    size_t conn1Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 conn1Id = CONN2ID(swConn);
                 swConn.getValue()->indicateFailure(Status(ErrorCodes::BadValue, "error"));
             });

    // Grab the second id
    size_t conn2Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 conn2Id = CONN2ID(swConn);
                 doneWith(swConn.getValue());
             });

    // Verify that we hit them, and that they're different
    ASSERT(conn1Id);
    ASSERT(conn2Id);
    ASSERT_NE(conn1Id, conn2Id);
}

/**
 * Verify that providing different host and ports gives you different
 * connections.
 */
TEST_F(ConnectionPoolTest, DifferentHostDifferentConn) {
    ConnectionPool pool(stdx::make_unique<PoolImpl>());

    // Conn 1 from port 30000
    size_t conn1Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort("localhost:30000"),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 conn1Id = CONN2ID(swConn);
                 doneWith(swConn.getValue());
             });

    // Conn 2 from port 30001
    size_t conn2Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort("localhost:30001"),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 conn2Id = CONN2ID(swConn);
                 doneWith(swConn.getValue());
             });

    // Hit them and not the same
    ASSERT(conn1Id);
    ASSERT(conn2Id);
    ASSERT_NE(conn1Id, conn2Id);
}

/**
 * Verify that not returning handle's to the pool spins up new connections.
 */
TEST_F(ConnectionPoolTest, DifferentConnWithoutReturn) {
    ConnectionPool pool(stdx::make_unique<PoolImpl>());

    // Get the first connection, move it out rather than letting it return
    ConnectionPool::ConnectionHandle conn1;
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT(swConn.isOK());
                 conn1 = std::move(swConn.getValue());
             });

    // Get the second connection, move it out rather than letting it return
    ConnectionPool::ConnectionHandle conn2;
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT(swConn.isOK());
                 conn2 = std::move(swConn.getValue());
             });

    // Verify that the two connections are different
    ASSERT_NE(conn1.get(), conn2.get());

    doneWith(conn1);
    doneWith(conn2);
}

/**
 * Verify that timing out on setup works as expected (a bad status is
 * returned).
 *
 * Note that the lack of pushSetup() calls delays the get.
 */
TEST_F(ConnectionPoolTest, TimeoutOnSetup) {
    ConnectionPool pool(stdx::make_unique<PoolImpl>());

    bool notOk = false;

    auto now = Date_t::now();

    PoolImpl::setNow(now);

    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) { notOk = !swConn.isOK(); });

    PoolImpl::setNow(now + Milliseconds(5000));

    ASSERT(notOk);
}

/**
 * Verify that refresh callbacks happen at the appropriate moments.
 */
TEST_F(ConnectionPoolTest, refreshHappens) {
    bool refreshedA = false;
    bool refreshedB = false;
    ConnectionImpl::pushRefresh([&]() {
        refreshedA = true;
        return Status::OK();
    });

    ConnectionImpl::pushRefresh([&]() {
        refreshedB = true;
        return Status::OK();
    });

    ConnectionPool::Options options;
    options.refreshRequirement = Milliseconds(1000);
    ConnectionPool pool(stdx::make_unique<PoolImpl>(), options);

    auto now = Date_t::now();

    PoolImpl::setNow(now);

    // Get a connection
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT(swConn.isOK());
                 doneWith(swConn.getValue());
             });

    // After 1 second, one refresh has occurred
    PoolImpl::setNow(now + Milliseconds(1000));
    ASSERT(refreshedA);
    ASSERT(!refreshedB);

    // After 1.5 seconds, the second refresh still hasn't triggered
    PoolImpl::setNow(now + Milliseconds(1500));
    ASSERT(!refreshedB);

    // At 2 seconds, the second refresh has triggered
    PoolImpl::setNow(now + Milliseconds(2000));
    ASSERT(refreshedB);
}

/**
 * Verify that refresh can timeout.
 */
TEST_F(ConnectionPoolTest, refreshTimeoutHappens) {
    ConnectionPool::Options options;
    options.refreshRequirement = Milliseconds(1000);
    options.refreshTimeout = Milliseconds(2000);
    ConnectionPool pool(stdx::make_unique<PoolImpl>(), options);

    auto now = Date_t::now();

    PoolImpl::setNow(now);

    size_t conn1Id = 0;

    // Grab a connection and verify it's good
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 conn1Id = CONN2ID(swConn);
                 doneWith(swConn.getValue());
             });

    PoolImpl::setNow(now + Milliseconds(500));

    size_t conn2Id = 0;
    // Make sure we still get the first one
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 conn2Id = CONN2ID(swConn);
                 doneWith(swConn.getValue());
             });
    ASSERT_EQ(conn1Id, conn2Id);

    // This should trigger a refresh, but not time it out. So now we have one
    // connection sitting in refresh.
    PoolImpl::setNow(now + Milliseconds(2000));
    bool reachedA = false;

    // This will wait because we have a refreshing connection, so it'll wait to
    // see if that pans out. In this case, we'll get a failure on timeout.
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(10000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT(!swConn.isOK());

                 reachedA = true;
             });
    ASSERT(!reachedA);

    // Let the refresh timeout
    PoolImpl::setNow(now + Milliseconds(4000));

    bool reachedB = false;

    // Make sure we can get a new connection
    pool.get(HostAndPort(),
             Milliseconds(1000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT_NE(CONN2ID(swConn), conn1Id);
                 reachedB = true;
                 doneWith(swConn.getValue());
             });

    ASSERT(reachedA);
    ASSERT(reachedB);
}

/**
 * Verify that requests are served in expiration order, not insertion order
 */
TEST_F(ConnectionPoolTest, requestsServedByUrgency) {
    ConnectionPool pool(stdx::make_unique<PoolImpl>());

    bool reachedA = false;
    bool reachedB = false;

    ConnectionPool::ConnectionHandle conn;

    pool.get(HostAndPort(),
             Milliseconds(2000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT(swConn.isOK());

                 reachedA = true;
                 doneWith(swConn.getValue());
             });

    pool.get(HostAndPort(),
             Milliseconds(1000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT(swConn.isOK());

                 reachedB = true;

                 conn = std::move(swConn.getValue());
             });

    ConnectionImpl::pushSetup(Status::OK());

    // Note thate we hit the 1 second request, but not the 2 second
    ASSERT(reachedB);
    ASSERT(!reachedA);

    doneWith(conn);
    conn.reset();

    // Now that we've returned the connection, we see the second has been
    // called
    ASSERT(reachedA);
}

/**
 * Verify that we respect maxConnections
 */
TEST_F(ConnectionPoolTest, maxPoolRespected) {
    ConnectionPool::Options options;
    options.minConnections = 1;
    options.maxConnections = 2;
    ConnectionPool pool(stdx::make_unique<PoolImpl>(), options);

    ConnectionPool::ConnectionHandle conn1;
    ConnectionPool::ConnectionHandle conn2;
    ConnectionPool::ConnectionHandle conn3;

    // Make 3 requests, each which keep their connection (don't return it to
    // the pool)
    pool.get(HostAndPort(),
             Milliseconds(3000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT(swConn.isOK());

                 conn3 = std::move(swConn.getValue());
             });
    pool.get(HostAndPort(),
             Milliseconds(2000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT(swConn.isOK());

                 conn2 = std::move(swConn.getValue());
             });
    pool.get(HostAndPort(),
             Milliseconds(1000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT(swConn.isOK());

                 conn1 = std::move(swConn.getValue());
             });

    ConnectionImpl::pushSetup(Status::OK());
    ConnectionImpl::pushSetup(Status::OK());
    ConnectionImpl::pushSetup(Status::OK());

    // Note that only two have run
    ASSERT(conn1);
    ASSERT(conn2);
    ASSERT(!conn3);

    // Return 1
    ConnectionPool::ConnectionInterface* conn1Ptr = conn1.get();
    doneWith(conn1);
    conn1.reset();

    // Verify that it's the one that pops out for request 3
    ASSERT_EQ(conn1Ptr, conn3.get());

    doneWith(conn2);
    doneWith(conn3);
}

/**
 * Verify that minConnections is respected
 */
TEST_F(ConnectionPoolTest, minPoolRespected) {
    ConnectionPool::Options options;
    options.minConnections = 2;
    options.maxConnections = 3;
    options.refreshRequirement = Milliseconds(1000);
    options.refreshTimeout = Milliseconds(2000);
    ConnectionPool pool(stdx::make_unique<PoolImpl>(), options);

    auto now = Date_t::now();

    PoolImpl::setNow(now);

    ConnectionPool::ConnectionHandle conn1;
    ConnectionPool::ConnectionHandle conn2;
    ConnectionPool::ConnectionHandle conn3;

    // Grab one connection without returning it
    pool.get(HostAndPort(),
             Milliseconds(1000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT(swConn.isOK());

                 conn1 = std::move(swConn.getValue());
             });

    bool reachedA = false;
    bool reachedB = false;
    bool reachedC = false;

    ConnectionImpl::pushSetup([&]() {
        reachedA = true;
        return Status::OK();
    });
    ConnectionImpl::pushSetup([&]() {
        reachedB = true;
        return Status::OK();
    });
    ConnectionImpl::pushSetup([&]() {
        reachedC = true;
        return Status::OK();
    });

    // Verify that two setups were invoked, even without two requests (the
    // minConnections == 2)
    ASSERT(reachedA);
    ASSERT(reachedB);
    ASSERT(!reachedC);

    // Two more get's without returns
    pool.get(HostAndPort(),
             Milliseconds(2000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT(swConn.isOK());

                 conn2 = std::move(swConn.getValue());
             });
    pool.get(HostAndPort(),
             Milliseconds(3000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT(swConn.isOK());

                 conn3 = std::move(swConn.getValue());
             });

    ASSERT(conn2);
    ASSERT(conn3);

    reachedA = false;
    reachedB = false;
    reachedC = false;

    ConnectionImpl::pushRefresh([&]() {
        reachedA = true;
        return Status::OK();
    });
    ConnectionImpl::pushRefresh([&]() {
        reachedB = true;
        return Status::OK();
    });
    ConnectionImpl::pushRefresh([&]() {
        reachedC = true;
        return Status::OK();
    });

    // Return each connection over 1, 2 and 3 ms
    PoolImpl::setNow(now + Milliseconds(1));
    doneWith(conn1);
    conn1.reset();

    PoolImpl::setNow(now + Milliseconds(2));
    doneWith(conn2);
    conn2.reset();

    PoolImpl::setNow(now + Milliseconds(3));
    doneWith(conn3);
    conn3.reset();

    // Jump 5 seconds and verify that refreshes only two refreshes occurred
    PoolImpl::setNow(now + Milliseconds(5000));

    ASSERT(reachedA);
    ASSERT(reachedB);
    ASSERT(!reachedC);
}


/**
 * Verify that the hostTimeout is respected. This implies that an idle
 * hostAndPort drops it's connections.
 */
TEST_F(ConnectionPoolTest, hostTimeoutHappens) {
    ConnectionPool::Options options;
    options.refreshRequirement = Milliseconds(5000);
    options.refreshTimeout = Milliseconds(5000);
    options.hostTimeout = Milliseconds(1000);
    ConnectionPool pool(stdx::make_unique<PoolImpl>(), options);

    auto now = Date_t::now();

    PoolImpl::setNow(now);

    size_t connId = 0;

    bool reachedA = false;
    // Grab 1 connection and return it
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 connId = CONN2ID(swConn);
                 reachedA = true;
                 doneWith(swConn.getValue());
             });

    ASSERT(reachedA);

    // Jump pass the hostTimeout
    PoolImpl::setNow(now + Milliseconds(1000));

    bool reachedB = false;

    // Verify that a new connection was spawned
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT_NE(connId, CONN2ID(swConn));
                 reachedB = true;
                 doneWith(swConn.getValue());
             });

    ASSERT(reachedB);
}


/**
 * Verify that the hostTimeout happens, but that continued gets delay
 * activation.
 */
TEST_F(ConnectionPoolTest, hostTimeoutHappensMoreGetsDelay) {
    ConnectionPool::Options options;
    options.refreshRequirement = Milliseconds(5000);
    options.refreshTimeout = Milliseconds(5000);
    options.hostTimeout = Milliseconds(1000);
    ConnectionPool pool(stdx::make_unique<PoolImpl>(), options);

    auto now = Date_t::now();

    PoolImpl::setNow(now);

    size_t connId = 0;

    bool reachedA = false;

    // Grab and return
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 connId = CONN2ID(swConn);
                 reachedA = true;
                 doneWith(swConn.getValue());
             });
    ASSERT(reachedA);

    // Jump almost up to the hostTimeout
    PoolImpl::setNow(now + Milliseconds(999));

    bool reachedB = false;
    // Same connection
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT_EQ(connId, CONN2ID(swConn));
                 reachedB = true;
                 doneWith(swConn.getValue());
             });
    ASSERT(reachedB);

    // Now we've timed out
    PoolImpl::setNow(now + Milliseconds(2000));

    bool reachedC = false;
    // Different id
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT_NE(connId, CONN2ID(swConn));
                 reachedC = true;
                 doneWith(swConn.getValue());
             });

    ASSERT(reachedC);
}


/**
 * Verify that the hostTimeout happens and that having a connection checked out
 * delays things
 */
TEST_F(ConnectionPoolTest, hostTimeoutHappensCheckoutDelays) {
    ConnectionPool::Options options;
    options.refreshRequirement = Milliseconds(5000);
    options.refreshTimeout = Milliseconds(5000);
    options.hostTimeout = Milliseconds(1000);
    ConnectionPool pool(stdx::make_unique<PoolImpl>(), options);

    auto now = Date_t::now();

    PoolImpl::setNow(now);

    ConnectionPool::ConnectionHandle conn1;
    size_t conn1Id = 0;
    size_t conn2Id = 0;

    // save 1 connection
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 conn1Id = CONN2ID(swConn);
                 conn1 = std::move(swConn.getValue());
             });

    ASSERT(conn1Id);

    // return the second
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 conn2Id = CONN2ID(swConn);
                 doneWith(swConn.getValue());
             });

    ASSERT(conn2Id);

    // hostTimeout has passed
    PoolImpl::setNow(now + Milliseconds(1000));

    bool reachedA = false;

    // conn 2 is still there
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT_EQ(conn2Id, CONN2ID(swConn));
                 reachedA = true;
                 doneWith(swConn.getValue());
             });

    ASSERT(reachedA);

    // return conn 1
    doneWith(conn1);
    conn1.reset();

    // expire the pool
    PoolImpl::setNow(now + Milliseconds(2000));

    bool reachedB = false;

    // make sure that this is a new id
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT_NE(conn1Id, CONN2ID(swConn));
                 ASSERT_NE(conn2Id, CONN2ID(swConn));
                 reachedB = true;
                 doneWith(swConn.getValue());
             });
    ASSERT(reachedB);
}

/**
 * Verify that drop connections works
 */
TEST_F(ConnectionPoolTest, dropConnections) {
    ConnectionPool::Options options;

    // ensure that only 1 connection is floating around
    options.maxConnections = 1;
    options.refreshRequirement = Seconds(1);
    options.refreshTimeout = Seconds(2);
    ConnectionPool pool(stdx::make_unique<PoolImpl>(), options);

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    // Grab the first connection id
    size_t conn1Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 conn1Id = CONN2ID(swConn);
                 doneWith(swConn.getValue());
             });
    ASSERT(conn1Id);

    // Grab it and this time keep it out of the pool
    ConnectionPool::ConnectionHandle handle;
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT_EQ(CONN2ID(swConn), conn1Id);
                 handle = std::move(swConn.getValue());
             });

    ASSERT(handle);

    bool reachedA = false;

    // Queue up a request. This won't fire until we drop connections, then it
    // will fail.
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT(!swConn.isOK());
                 reachedA = true;
             });
    ASSERT(!reachedA);

    // fails the previous get
    pool.dropConnections(HostAndPort());

    ASSERT(reachedA);

    // return the connection
    doneWith(handle);
    handle.reset();

    // Make sure that a new connection request properly disposed of the gen1
    // connection
    size_t conn2Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 conn2Id = CONN2ID(swConn);
                 ASSERT_NE(conn2Id, conn1Id);
                 doneWith(swConn.getValue());
             });
    ASSERT(conn2Id);

    // Push conn2 into refresh
    PoolImpl::setNow(now + Milliseconds(1500));

    // drop the connections
    pool.dropConnections(HostAndPort());

    // refresh still pending
    PoolImpl::setNow(now + Milliseconds(2500));

    // Verify that a new connection came out, despite the gen2 connection still
    // being pending
    bool reachedB = false;
    ConnectionImpl::pushSetup(Status::OK());
    pool.get(HostAndPort(),
             Milliseconds(5000),
             [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                 ASSERT_NE(CONN2ID(swConn), conn2Id);
                 reachedB = true;
                 doneWith(swConn.getValue());
             });

    ASSERT(reachedB);
}

}  // namespace connection_pool_test_details
}  // namespace executor
}  // namespace mongo
