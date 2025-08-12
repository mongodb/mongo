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

#include "mongo/executor/connection_pool.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/connection_pool_test_fixture.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/executor_test_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"

#include <algorithm>
#include <array>
#include <memory>
#include <random>
#include <ratio>
#include <set>
#include <stack>
#include <tuple>
#include <type_traits>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {
namespace executor {
namespace connection_pool_test_details {

class ConnectionPoolTest : public unittest::Test {
public:
    constexpr static Milliseconds kNoTimeout = Milliseconds{-1};

protected:
    void setUp() override {}

    void tearDown() override {
        if (_pool) {
            _pool->shutdown();
        }

        ConnectionImpl::clear();
        TimerImpl::clear();
    }

    auto makePool(ConnectionPool::Options options = {}) {
        _pool = std::make_shared<ConnectionPool>(
            std::make_shared<PoolImpl>(_executor), "test pool", options);
        return _pool;
    }

    void dropPool() {
        _pool = {};
    }

    /**
     * Get from a pool with out-of-line execution and return the future for a connection
     *
     * Since the InlineOutOfLineExecutor starts running on the same thread once schedule is called,
     * this function allows us to avoid deadlocks with get(), which is the only public function that
     * calls schedule while holding a lock. In normal operation, the OutOfLineExecutor is actually
     * out of line, and this contrivance isn't necessary.
     */
    template <typename... Args>
    auto getFromPool(Args&&... args) {
        return ExecutorFuture(_executor)
            .then([pool = _pool, args...]() { return pool->get(args...); })
            .semi();
    }

    void doneWith(ConnectionPool::ConnectionHandle& conn) {
        dynamic_cast<ConnectionImpl*>(conn.get())->indicateSuccess();

        ExecutorFuture(_executor).getAsync([conn = std::move(conn)](auto) {});
    }

    void doneWithError(ConnectionPool::ConnectionHandle& conn, Status error) {
        dynamic_cast<ConnectionImpl*>(conn.get())->indicateFailure(error);

        ExecutorFuture(_executor).getAsync([conn = std::move(conn)](auto) {});
    }

    using StatusWithConn = StatusWith<ConnectionPool::ConnectionHandle>;

    auto getId(const ConnectionPool::ConnectionHandle& conn) {
        return dynamic_cast<ConnectionImpl*>(conn.get())->id();
    }
    auto verifyAndGetId(StatusWithConn& swConn) {
        ASSERT(swConn.isOK());
        auto& conn = swConn.getValue();
        return getId(conn);
    }

    template <typename Ptr>
    void dropConnectionsTest(std::shared_ptr<ConnectionPool> const& pool, Ptr t);

    /**
     * Helper for asserting connection pool time-out behaviours.
     *
     * Gets a connection from a new pool with a timeout duration,
     * asserting the connection times out with the appropriate expected code
     * associated with the matcher.
     *
     * The controller's refresh timeout is set to 250ms.
     */
    void assertTimeoutHelper(Milliseconds acquisitionTimeout, ErrorCodes::Error errorCode) {
        ConnectionPool::Options options;
        options.refreshTimeout = Milliseconds{250};
        auto pool = makePool(options);
        auto now = Date_t::now();

        PoolImpl::setNow(now);

        StatusWith<ConnectionPool::ConnectionHandle> connectionHandle{nullptr};
        pool->get_forTest(HostAndPort(),
                          acquisitionTimeout,
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              connectionHandle = std::move(swConn);
                          });

        // Ensure only one timeout fires.
        auto minTimeout = std::min(options.refreshTimeout, acquisitionTimeout);

        PoolImpl::setNow(now + minTimeout);

        ASSERT(!connectionHandle.isOK());
        ASSERT_EQ(connectionHandle.getStatus().code(), errorCode);
    }

private:
    std::shared_ptr<OutOfLineExecutor> _executor = InlineQueuedCountingExecutor::make();
    std::shared_ptr<ConnectionPool> _pool;
};

TEST_F(ConnectionPoolTest, CheckRejectedConnectionRequest) {
    ConnectionPool::Options opts;
    opts.connectionRequestsMaxQueueDepth = 1;
    auto pool = makePool(opts);

    FailPointEnableBlock fpb("connectionPoolDoesNotFulfillRequests");
    auto conn1Fut = getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds(1));
    ASSERT_FALSE(conn1Fut.isReady());

    auto fut = getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds(1));
    ASSERT_TRUE(fut.isReady());
    ASSERT_THROWS_CODE(
        std::move(fut).get(), DBException, ErrorCodes::PooledConnectionAcquisitionRejected);
}

/**
 * Verify that the limit on the size of connection requests queue
 * is enforced properly.
 */
TEST_F(ConnectionPoolTest, CheckRejectedConnectionRequestBasic) {
    auto pool = makePool();
    FailPointEnableBlock fpb("connectionPoolRejectsConnectionRequests");
    auto fut = getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds(1));
    ASSERT_TRUE(fut.isReady());
    ASSERT_THROWS_CODE(
        std::move(fut).get(), DBException, ErrorCodes::PooledConnectionAcquisitionRejected);
}

TEST_F(ConnectionPoolTest, StatsTest) {
    constexpr auto numConnections = 3;
    auto hosts = std::vector<HostAndPort>(
        {HostAndPort("host1:123"), HostAndPort("host2:456"), HostAndPort("host3:789")});

    auto pool = makePool();
    auto createAndUseConnection = [&](HostAndPort host) {
        ConnectionImpl::pushSetup(Status::OK());
        pool->get_forTest(
            host, Milliseconds(5000), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                doneWith(swConn.getValue());
            });
        pool->setKeepOpen(host, false);
    };
    auto getStats = [&] {
        ConnectionPoolStats stats;
        pool->appendConnectionStats(&stats);
        return stats;
    };

    for (int i = 0; i < numConnections; ++i) {
        createAndUseConnection(hosts.at(i));
    }

    ASSERT_EQ(getStats().totalCreated, numConnections);

    // After dropping connections to the first host, totalCreated stat should not change.
    pool->dropConnections(hosts.at(0));
    ASSERT_EQ(getStats().totalCreated, numConnections);

    // After dropping connections to all hosts, totalCreated stat should not change.
    pool->dropConnections();
    ASSERT_EQ(getStats().totalCreated, numConnections);

    // Opening a connection to an old host should update created stats accordingly.
    createAndUseConnection(hosts.at(0));
    auto stats = getStats();
    ASSERT_EQ(stats.statsByHost[hosts.at(0)].created, 2);
    ASSERT_EQ(getStats().totalCreated, numConnections + 1);

    // And dropping the connection again should not change totalCreated.
    pool->dropConnections();
    ASSERT_EQ(getStats().totalCreated, numConnections + 1);
}

/**
 * Verify that we get the same connection if we grab one, return it and grab
 * another.
 */
TEST_F(ConnectionPoolTest, SameConn) {
    auto pool = makePool();

    // Grab and stash an id for the first request
    size_t conn1Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          conn1Id = verifyAndGetId(swConn);
                          doneWith(swConn.getValue());
                      });

    // Grab and stash an id for the second request
    size_t conn2Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          conn2Id = verifyAndGetId(swConn);
                          doneWith(swConn.getValue());
                      });

    // Verify that we hit them, and that they're the same
    ASSERT(conn1Id);
    ASSERT(conn2Id);
    ASSERT_EQ(conn1Id, conn2Id);
}

/**
 * Verify that connections are obtained in MRU order.
 */
TEST_F(ConnectionPoolTest, ConnectionsAreAcquiredInMRUOrder) {
    auto pool = makePool();
    std::random_device rd;
    std::mt19937 rng(rd());

    // Obtain a set of connections
    constexpr size_t kSize = 100;
    std::vector<ConnectionPool::ConnectionHandle> connections;
    std::vector<unittest::ThreadAssertionMonitor> monitors(kSize);

    // Ensure that no matter how we leave the test, we mark any
    // checked out connections as OK before implicity returning them
    // to the pool by destroying the 'connections' vector. Otherwise,
    // this test would cause an invariant failure instead of a normal
    // test failure if it fails, which would be confusing.
    const ScopeGuard guard([&] {
        while (!connections.empty()) {
            try {
                ConnectionPool::ConnectionHandle conn = std::move(connections.back());
                connections.pop_back();
                doneWith(conn);
            } catch (...) {
            }
        }
    });

    std::uniform_int_distribution<> dist{0, 1};
    for (size_t i = 0; i != kSize; ++i) {
        ConnectionImpl::pushSetup(Status::OK());
        auto cb = [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            monitors[i].exec([&]() {
                ASSERT(swConn.isOK());
                connections.push_back(std::move(swConn.getValue()));
                monitors[i].notifyDone();
            });
        };
        auto timeout = Milliseconds(5000);

        // Randomly lease or check out connection.
        if (dist(rng)) {
            pool->get_forTest(HostAndPort(), timeout, cb);
        } else {
            pool->lease_forTest(HostAndPort(), timeout, cb);
        }
    }

    for (auto& monitor : monitors) {
        monitor.wait();
    }

    ASSERT_EQ(connections.size(), kSize);

    // Shuffle them into a random order
    std::shuffle(connections.begin(), connections.end(), rng);

    // Return them to the pool in that random order, recording IDs in a stack
    std::stack<size_t> ids;
    while (!connections.empty()) {
        ConnectionPool::ConnectionHandle conn = std::move(connections.back());
        connections.pop_back();
        ids.push(static_cast<ConnectionImpl*>(conn.get())->id());
        doneWith(conn);
    }
    ASSERT_EQ(ids.size(), kSize);

    // Replace the thread monitors with fresh ones.
    monitors = std::vector<unittest::ThreadAssertionMonitor>(kSize);

    // Re-obtain the connections. They should come back in the same order
    // as the IDs in the stack, since the pool returns them in MRU order.
    for (size_t i = 0; i != kSize; ++i) {
        ConnectionImpl::pushSetup(Status::OK());
        auto cb = [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            monitors[i].exec([&]() {
                ASSERT(swConn.isOK());
                const auto id = verifyAndGetId(swConn);
                connections.push_back(std::move(swConn.getValue()));
                ASSERT_EQ(id, ids.top());
                ids.pop();
                monitors[i].notifyDone();
            });
        };
        auto timeout = Milliseconds(5000);

        // Randomly lease or check out connection.
        if (dist(rng)) {
            pool->get_forTest(HostAndPort(), timeout, cb);
        } else {
            pool->lease_forTest(HostAndPort(), timeout, cb);
        }
    }

    for (auto& monitor : monitors) {
        monitor.wait();
    }

    ASSERT(ids.empty());
}

/**
 * Verify that recently used connections are not purged.
 */
TEST_F(ConnectionPoolTest, ConnectionsNotUsedRecentlyArePurged) {
    ConnectionPool::Options options;
    options.minConnections = 0;
    options.refreshRequirement = Milliseconds(1000);
    options.refreshTimeout = Milliseconds(5000);
    options.hostTimeout = Minutes(1);
    auto pool = makePool(options);

    ASSERT_EQ(pool->getNumConnectionsPerHost(HostAndPort()), 0U);

    // Obtain a set of connections
    constexpr size_t kSize = 100;
    std::vector<ConnectionPool::ConnectionHandle> connections;
    std::vector<unittest::ThreadAssertionMonitor> monitors(kSize);

    // Ensure that no matter how we leave the test, we mark any
    // checked out connections as OK before implicity returning them
    // to the pool by destroying the 'connections' vector. Otherwise,
    // this test would cause an invariant failure instead of a normal
    // test failure if it fails, which would be confusing.
    const ScopeGuard guard([&] {
        while (!connections.empty()) {
            try {
                ConnectionPool::ConnectionHandle conn = std::move(connections.back());
                connections.pop_back();
                doneWith(conn);
            } catch (...) {
            }
        }
    });

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    // Check out kSize connections from the pool, and record their IDs in a set.
    std::set<size_t> original_ids;
    for (size_t i = 0; i != kSize; ++i) {
        ConnectionImpl::pushSetup(Status::OK());
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitors[i].exec([&]() {
                                  ASSERT(swConn.isOK());
                                  original_ids.insert(verifyAndGetId(swConn));
                                  connections.push_back(std::move(swConn.getValue()));
                                  monitors[i].notifyDone();
                              });
                          });
    }

    for (auto& monitor : monitors) {
        monitor.wait();
    }

    ASSERT_EQ(original_ids.size(), kSize);
    ASSERT_EQ(pool->getNumConnectionsPerHost(HostAndPort()), kSize);

    // Shuffle them into a random order
    std::random_device rd;
    std::mt19937 rng(rd());
    std::shuffle(connections.begin(), connections.end(), rng);

    // Return them to the pool in that random order.
    while (!connections.empty()) {
        ConnectionPool::ConnectionHandle conn = std::move(connections.back());
        connections.pop_back();
        doneWith(conn);
    }

    // Advance the time, but not enough to age out connections. We should still have them all.
    PoolImpl::setNow(now + Milliseconds(500));
    ASSERT_EQ(pool->getNumConnectionsPerHost(HostAndPort()), kSize);

    // Re-obtain a quarter of the connections, and record their IDs in a set.
    monitors = std::vector<unittest::ThreadAssertionMonitor>(kSize / 4);
    std::set<size_t> reacquired_ids;
    for (size_t i = 0; i < kSize / 4; ++i) {
        ConnectionImpl::pushSetup(Status::OK());
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitors[i].exec([&]() {
                                  ASSERT(swConn.isOK());
                                  reacquired_ids.insert(verifyAndGetId(swConn));
                                  connections.push_back(std::move(swConn.getValue()));
                                  monitors[i].notifyDone();
                              });
                          });
    }

    for (auto& monitor : monitors) {
        monitor.wait();
    }

    ASSERT_EQ(reacquired_ids.size(), kSize / 4);
    ASSERT(std::includes(
        original_ids.begin(), original_ids.end(), reacquired_ids.begin(), reacquired_ids.end()));
    ASSERT_EQ(pool->getNumConnectionsPerHost(HostAndPort()), kSize);

    // Put them right back in.
    while (!connections.empty()) {
        ConnectionPool::ConnectionHandle conn = std::move(connections.back());
        connections.pop_back();
        doneWith(conn);
    }

    // We should still have all of them in the pool
    ASSERT_EQ(pool->getNumConnectionsPerHost(HostAndPort()), kSize);

    // Advance across the host timeout for the 75 connections we
    // didn't use. Afterwards, the pool should contain only those
    // kSize/4 connections we used above.
    PoolImpl::setNow(now + Milliseconds(1000));
    ASSERT_EQ(pool->getNumConnectionsPerHost(HostAndPort()), kSize / 4);
}

/**
 * Verify that a failed connection isn't returned to the pool
 */
TEST_F(ConnectionPoolTest, FailedConnDifferentConn) {
    auto pool = makePool();

    // Grab the first connection and indicate that it failed
    size_t conn1Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          conn1Id = verifyAndGetId(swConn);
                          swConn.getValue()->indicateFailure(Status(ErrorCodes::BadValue, "error"));
                      });

    // Grab the second id
    size_t conn2Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          conn2Id = verifyAndGetId(swConn);
                          doneWith(swConn.getValue());
                      });

    // Verify that we hit them, and that they're different
    ASSERT(conn1Id);
    ASSERT(conn2Id);
    ASSERT_NE(conn1Id, conn2Id);
}

/**
 * Verify that a connection returned with an error indicating the remote
 * is unavailable drops current generation connections to that remote.
 */
TEST_F(ConnectionPoolTest, FailedHostDropsConns) {
    auto pool = makePool();

    ASSERT_EQ(pool->getNumConnectionsPerHost(HostAndPort()), 0U);

    constexpr size_t kSize = 100;
    std::vector<ConnectionPool::ConnectionHandle> connections;
    std::vector<unittest::ThreadAssertionMonitor> monitors(kSize);

    // Ensure that no matter how we leave the test, we mark any
    // checked out connections as OK before implicity returning them
    // to the pool by destroying the 'connections' vector. Otherwise,
    // this test would cause an invariant failure instead of a normal
    // test failure if it fails, which would be confusing.
    auto drainConnPool = [&] {
        while (!connections.empty()) {
            try {
                ConnectionPool::ConnectionHandle conn = std::move(connections.back());
                connections.pop_back();
                doneWith(conn);
            } catch (...) {
            }
        }
    };
    const ScopeGuard guard(drainConnPool);

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    // Check out kSize connections from the pool.
    for (size_t i = 0; i != kSize; ++i) {
        ConnectionImpl::pushSetup(Status::OK());
        auto cb = [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            monitors[i].exec([&]() {
                ASSERT(swConn.isOK());
                connections.push_back(std::move(swConn.getValue()));
                monitors[i].notifyDone();
            });
        };
        auto timeout = Milliseconds(5000);

        pool->get_forTest(HostAndPort(), timeout, cb);
    }

    for (auto& monitor : monitors) {
        monitor.wait();
    }

    ASSERT_EQ(pool->getNumConnectionsPerHost(HostAndPort()), kSize);

    // Return one connection with a network error.
    ConnectionPool::ConnectionHandle conn = std::move(connections.back());
    connections.pop_back();
    doneWithError(conn, {ErrorCodes::HostUnreachable, "error"});

    // We should still have all of the connections open, minus the one we just returned with an
    // error.
    ASSERT_EQ(pool->getNumConnectionsPerHost(HostAndPort()), kSize - 1);

    // Put the remaining connections back.
    drainConnPool();

    // They should all be discarded since the host should be marked as down
    // due to the connection returned with a network error.
    ASSERT_EQ(pool->getNumConnectionsPerHost(HostAndPort()), 0);
}

/**
 * Verify that a connection returned with an error that does _not_ indicate
 * the remote is unavailable does _not_ drop current generation connections to that remote.
 */
TEST_F(ConnectionPoolTest, OtherErrorsDontDropConns) {
    auto pool = makePool();

    ASSERT_EQ(pool->getNumConnectionsPerHost(HostAndPort()), 0U);

    constexpr size_t kSize = 100;
    std::vector<ConnectionPool::ConnectionHandle> connections;

    // Ensure that no matter how we leave the test, we mark any
    // checked out connections as OK before implicity returning them
    // to the pool by destroying the 'connections' vector. Otherwise,
    // this test would cause an invariant failure instead of a normal
    // test failure if it fails, which would be confusing.
    auto drainConnPool = [&] {
        while (!connections.empty()) {
            try {
                ConnectionPool::ConnectionHandle conn = std::move(connections.back());
                connections.pop_back();
                doneWith(conn);
            } catch (...) {
            }
        }
    };
    const ScopeGuard guard(drainConnPool);

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    auto checkOutConnections = [&] {
        std::vector<unittest::ThreadAssertionMonitor> monitors(kSize);
        for (size_t i = 0; i != kSize; ++i) {
            ConnectionImpl::pushSetup(Status::OK());
            auto cb = [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                monitors[i].exec([&]() {
                    ASSERT(swConn.isOK());
                    connections.push_back(std::move(swConn.getValue()));
                    monitors[i].notifyDone();
                });
            };
            auto timeout = Milliseconds(5000);

            pool->get_forTest(HostAndPort(), timeout, cb);
        }

        for (auto& monitor : monitors) {
            monitor.wait();
        }

        ASSERT_EQ(pool->getNumConnectionsPerHost(HostAndPort()), kSize);
    };

    // All three types of error that shouldn't result in us dropping connections - a non-network
    // error; a network timeout error, and a network error that we can isolate to a specific
    // connection.
    std::array<ErrorCodes::Error, 3> errors = {
        ErrorCodes::InternalError, ErrorCodes::NetworkTimeout, ErrorCodes::ConnectionError};
    for (size_t i = 0; i < errors.size(); ++i) {
        // Check out kSize connections from the pool.
        checkOutConnections();
        // Return one connection with a non-network error.
        ConnectionPool::ConnectionHandle conn = std::move(connections.back());
        connections.pop_back();
        doneWithError(conn, {errors[i], "error"});

        // We should still have all of the connections open, minus the one we just returned with an
        // error.
        ASSERT_EQ(pool->getNumConnectionsPerHost(HostAndPort()), kSize - 1);

        // Put the remaining connections back.
        drainConnPool();

        // They should all still be open.
        ASSERT_EQ(pool->getNumConnectionsPerHost(HostAndPort()), kSize - 1);
    }
}

/**
 * Verify that providing different host and ports gives you different
 * connections.
 */
TEST_F(ConnectionPoolTest, DifferentHostDifferentConn) {
    auto pool = makePool();

    // Conn 1 from port 30000
    size_t conn1Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort("localhost:30000"),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          conn1Id = verifyAndGetId(swConn);
                          doneWith(swConn.getValue());
                      });

    // Conn 2 from port 30001
    size_t conn2Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort("localhost:30001"),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          conn2Id = verifyAndGetId(swConn);
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
    auto pool = makePool();

    // Get the first connection, move it out rather than letting it return
    ConnectionPool::ConnectionHandle conn1;
    ConnectionImpl::pushSetup(Status::OK());
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  ASSERT(swConn.isOK());
                                  conn1 = std::move(swConn.getValue());
                              });
                          });
    });

    // Get the second connection, move it out rather than letting it return
    ConnectionPool::ConnectionHandle conn2;
    ConnectionImpl::pushSetup(Status::OK());

    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  ASSERT(swConn.isOK());
                                  conn2 = std::move(swConn.getValue());
                              });
                          });
    });

    // Verify that the two connections are different
    ASSERT_NE(conn1.get(), conn2.get());

    doneWith(conn1);
    doneWith(conn2);
}

/**
 * When the timeout duration comes from the parameter, the connection timeout status should
 * always be `PooledConnectionAcquisitionExceededTimeLimit`.
 */
TEST_F(ConnectionPoolTest, TimeoutOnAquisitionTimeout) {
    assertTimeoutHelper(
        /* timeout duration */ Milliseconds{100},
        /* expected timeout codes */ ErrorCodes::PooledConnectionAcquisitionExceededTimeLimit);
}

/**
 * When the timeout duration comes from controller refresh, the connection timeout status should
 * always be `HostUnreachable`.
 */
TEST_F(ConnectionPoolTest, TimeoutOnControllerRefresh) {
    assertTimeoutHelper(
        /* timeout duration */ Milliseconds{500},
        /* expected timeout codes */ ErrorCodes::HostUnreachable);
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
    auto pool = makePool(options);

    auto now = Date_t::now();

    PoolImpl::setNow(now);

    // Get a connection
    ConnectionImpl::pushSetup(Status::OK());
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  ASSERT(swConn.isOK());
                                  doneWith(swConn.getValue());
                              });
                          });
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
    auto pool = makePool(options);

    auto now = Date_t::now();

    PoolImpl::setNow(now);

    size_t conn1Id = 0;

    // Grab a connection and verify it's good
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        ConnectionImpl::pushSetup(Status::OK());
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  conn1Id = verifyAndGetId(swConn);
                                  doneWith(swConn.getValue());
                              });
                          });
        PoolImpl::setNow(now + Milliseconds(500));
    });

    size_t conn2Id = 0;

    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        // Make sure we still get the first one
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  conn2Id = verifyAndGetId(swConn);
                                  doneWith(swConn.getValue());
                              });
                          });
        ASSERT_EQ(conn1Id, conn2Id);
    });

    // This should trigger a refresh, but not time it out. So now we have one
    // connection sitting in refresh.
    PoolImpl::setNow(now + Milliseconds(2000));
    bool reachedA = false;

    // This will wait because we have a refreshing connection, so it'll wait to
    // see if that pans out. In this case, we'll get a failure on timeout.
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        ConnectionImpl::pushSetup(Status::OK());
        pool->get_forTest(HostAndPort(),
                          Milliseconds(1000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  ASSERT(!swConn.isOK());

                                  reachedA = true;
                              });
                          });
        ASSERT(!reachedA);
        PoolImpl::setNow(now + Milliseconds(3000));

        // Let the refresh timeout
        PoolImpl::setNow(now + Milliseconds(4000));
    });
    bool reachedB = false;

    // Make sure we can get a new connection
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(1000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  ASSERT_NE(verifyAndGetId(swConn), conn1Id);
                                  reachedB = true;
                                  doneWith(swConn.getValue());
                              });
                          });
    });

    ASSERT(reachedA);
    ASSERT(reachedB);
}

/**
 * Verify that requests are served in expiration order, not insertion order
 */
TEST_F(ConnectionPoolTest, requestsServedByUrgency) {
    auto pool = makePool();

    bool reachedA = false;
    bool reachedB = false;

    ConnectionPool::ConnectionHandle conn;
    unittest::ThreadAssertionMonitor c1;
    pool->get_forTest(HostAndPort(),
                      Milliseconds(2000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          c1.exec([&]() {
                              ASSERT(swConn.isOK());

                              reachedA = true;
                              doneWith(swConn.getValue());
                              c1.notifyDone();
                          });
                      });
    unittest::ThreadAssertionMonitor c2;
    pool->get_forTest(HostAndPort(),
                      Milliseconds(1000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          c2.exec([&]() {
                              ASSERT(swConn.isOK());

                              reachedB = true;

                              conn = std::move(swConn.getValue());
                              c2.notifyDone();
                          });
                      });

    ConnectionImpl::pushSetup(Status::OK());

    // Note thate we hit the 1 second request, but not the 2 second
    c2.wait();
    ASSERT(reachedB);
    ASSERT(!reachedA);

    doneWith(conn);

    // Now that we've returned the connection, we see the second has been
    // called
    c1.wait();
    ASSERT(reachedA);
}

/**
 * Verify that we respect maxConnections
 */
TEST_F(ConnectionPoolTest, maxPoolRespected) {
    ConnectionPool::Options options;
    options.minConnections = 1;
    options.maxConnections = 2;
    auto pool = makePool(options);

    ConnectionPool::ConnectionHandle conn1;
    ConnectionPool::ConnectionHandle conn2;
    ConnectionPool::ConnectionHandle conn3;

    // Make 3 requests, each which keep their connection (don't return it to
    // the pool)
    unittest::ThreadAssertionMonitor c3;
    pool->get_forTest(HostAndPort(),
                      Milliseconds(3000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          c3.exec([&]() {
                              ASSERT(swConn.isOK());

                              conn3 = std::move(swConn.getValue());
                              c3.notifyDone();
                          });
                      });
    unittest::ThreadAssertionMonitor c2;
    pool->get_forTest(HostAndPort(),
                      Milliseconds(2000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          c2.exec([&]() {
                              ASSERT(swConn.isOK());

                              conn2 = std::move(swConn.getValue());
                              c2.notifyDone();
                          });
                      });
    unittest::ThreadAssertionMonitor c1;
    pool->get_forTest(HostAndPort(),
                      Milliseconds(1000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          c1.exec([&]() {
                              ASSERT(swConn.isOK());

                              conn1 = std::move(swConn.getValue());
                              c1.notifyDone();
                          });
                      });

    ConnectionImpl::pushSetup(Status::OK());
    c1.wait();
    ConnectionImpl::pushSetup(Status::OK());
    c2.wait();
    ConnectionImpl::pushSetup(Status::OK());

    // Note that only two have run
    ASSERT(conn1);
    ASSERT(conn2);
    ASSERT(!conn3);

    // Return 1
    ConnectionPool::ConnectionInterface* conn1Ptr = conn1.get();
    doneWith(conn1);

    // Verify that it's the one that pops out for request 3
    c3.wait();
    ASSERT_EQ(conn1Ptr, conn3.get());

    doneWith(conn2);
    doneWith(conn3);
}

/**
 * Verify that we respect maxConnecting
 */
TEST_F(ConnectionPoolTest, maxConnectingRespected) {
    ConnectionPool::Options options;
    options.minConnections = 1;
    options.maxConnecting = 2;
    auto pool = makePool(options);

    ConnectionPool::ConnectionHandle conn1;
    ConnectionPool::ConnectionHandle conn2;
    ConnectionPool::ConnectionHandle conn3;

    // Make 3 requests, each which keep their connection (don't return it to
    // the pool)
    unittest::ThreadAssertionMonitor c3;
    pool->get_forTest(HostAndPort(),
                      Milliseconds(3000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          c3.exec([&]() {
                              ASSERT(swConn.isOK());

                              conn3 = std::move(swConn.getValue());
                              c3.notifyDone();
                          });
                      });
    unittest::ThreadAssertionMonitor c2;
    pool->get_forTest(HostAndPort(),
                      Milliseconds(2000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          c2.exec([&]() {
                              ASSERT(swConn.isOK());

                              conn2 = std::move(swConn.getValue());
                              c2.notifyDone();
                          });
                      });
    unittest::ThreadAssertionMonitor c1;
    pool->get_forTest(HostAndPort(),
                      Milliseconds(1000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          c1.exec([&]() {
                              ASSERT(swConn.isOK());

                              conn1 = std::move(swConn.getValue());
                              c1.notifyDone();
                          });
                      });

    ASSERT_EQ(ConnectionImpl::setupQueueDepth(), 2u);
    ConnectionImpl::pushSetup(Status::OK());
    c1.wait();
    ASSERT_EQ(ConnectionImpl::setupQueueDepth(), 2u);
    ConnectionImpl::pushSetup(Status::OK());
    c2.wait();
    ASSERT_EQ(ConnectionImpl::setupQueueDepth(), 1u);
    ConnectionImpl::pushSetup(Status::OK());
    c3.wait();
    ASSERT_EQ(ConnectionImpl::setupQueueDepth(), 0u);

    ASSERT(conn1);
    ASSERT(conn2);
    ASSERT(conn3);

    ASSERT_NE(conn1.get(), conn2.get());
    ASSERT_NE(conn2.get(), conn3.get());
    ASSERT_NE(conn1.get(), conn3.get());

    doneWith(conn1);
    doneWith(conn2);
    doneWith(conn3);
}

/**
 * Verify that refresh callbacks block new connections, then trigger new connection spawns after
 * they return
 */
TEST_F(ConnectionPoolTest, maxConnectingWithRefresh) {
    ConnectionPool::Options options;
    options.maxConnecting = 1;
    options.refreshRequirement = Milliseconds(1000);
    auto pool = makePool(options);

    auto now = Date_t::now();

    PoolImpl::setNow(now);

    // Get a connection
    ConnectionImpl::pushSetup(Status::OK());
    unittest::ThreadAssertionMonitor c1;
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          c1.exec([&]() {
                              ASSERT(swConn.isOK());
                              doneWith(swConn.getValue());
                              c1.notifyDone();
                          });
                      });

    ASSERT_EQ(ConnectionImpl::refreshQueueDepth(), 0u);

    // After 1 second, one refresh has queued
    PoolImpl::setNow(now + Milliseconds(1000));
    ASSERT_EQ(ConnectionImpl::refreshQueueDepth(), 1u);

    bool reachedA = false;

    // Try to get another connection
    unittest::ThreadAssertionMonitor c2;
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          c2.exec([&]() {
                              ASSERT(swConn.isOK());
                              doneWith(swConn.getValue());
                              reachedA = true;
                              c2.notifyDone();
                          });
                      });

    ASSERT_EQ(ConnectionImpl::setupQueueDepth(), 0u);
    ASSERT(!reachedA);
    c1.wait();
    ConnectionImpl::pushRefresh(Status::OK());
    ASSERT_EQ(ConnectionImpl::refreshQueueDepth(), 0u);
    ASSERT(reachedA);
    c2.wait();
}

/**
 * Verify that refreshes block new connects, but don't themselves respect maxConnecting
 */
TEST_F(ConnectionPoolTest, maxConnectingWithMultipleRefresh) {
    ConnectionPool::Options options;
    options.maxConnecting = 2;
    options.minConnections = 3;
    options.refreshRequirement = Milliseconds(1000);
    auto pool = makePool(options);

    auto now = Date_t::now();

    PoolImpl::setNow(now);

    // Get us spun up to 3 connections in the pool
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  ASSERT(swConn.isOK());
                                  doneWith(swConn.getValue());
                              });
                          });
        ASSERT_EQ(ConnectionImpl::setupQueueDepth(), 2u);
        ConnectionImpl::pushSetup(Status::OK());
    });
    ASSERT_EQ(ConnectionImpl::setupQueueDepth(), 2u);
    ConnectionImpl::pushSetup(Status::OK());
    ConnectionImpl::pushSetup(Status::OK());
    ASSERT_EQ(ConnectionImpl::setupQueueDepth(), 0u);

    // Force more than two connections into refresh
    PoolImpl::setNow(now + Milliseconds(1500));
    ASSERT_EQ(ConnectionImpl::refreshQueueDepth(), 3u);

    std::array<ConnectionPool::ConnectionHandle, 5> conns;
    std::array<unittest::ThreadAssertionMonitor, 5> ams;

    // Start 5 new requests
    for (size_t i = 0; i < conns.size(); ++i) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(static_cast<int>(1000 + i)),
                          [&conns, &ams, i](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              ams[i].exec([&]() {
                                  ASSERT(swConn.isOK());
                                  conns[i] = std::move(swConn.getValue());
                                  ams[i].notifyDone();
                              });
                          });
    }

    auto firstNBound = [&](size_t n) {
        for (size_t i = 0; i < n; ++i) {
            ASSERT(conns[i]);
        }
        for (size_t i = n; i < conns.size(); ++i) {
            ASSERT_FALSE(conns[i]);
        }
    };

    // None have started connecting
    ASSERT_EQ(ConnectionImpl::refreshQueueDepth(), 3u);
    ASSERT_EQ(ConnectionImpl::setupQueueDepth(), 0u);
    firstNBound(0);

    // After one refresh, one refreshed connection gets handed out
    ConnectionImpl::pushRefresh(Status::OK());
    ASSERT_EQ(ConnectionImpl::refreshQueueDepth(), 2u);
    ASSERT_EQ(ConnectionImpl::setupQueueDepth(), 0u);
    ams[0].wait();
    firstNBound(1);

    // After two refresh, one enters the setup queue, one refreshed connection gets handed out
    ConnectionImpl::pushRefresh(Status::OK());
    ASSERT_EQ(ConnectionImpl::refreshQueueDepth(), 1u);
    ASSERT_EQ(ConnectionImpl::setupQueueDepth(), 1u);
    ams[1].wait();
    firstNBound(2);

    // After three refresh, we're done refreshing. Two queued in setup
    ConnectionImpl::pushRefresh(Status::OK());
    ASSERT_EQ(ConnectionImpl::refreshQueueDepth(), 0u);
    ASSERT_EQ(ConnectionImpl::setupQueueDepth(), 2u);
    ams[2].wait();
    firstNBound(3);

    // now pushing setup gets us a new connection
    ConnectionImpl::pushSetup(Status::OK());
    ASSERT_EQ(ConnectionImpl::setupQueueDepth(), 1u);
    ams[3].wait();
    firstNBound(4);

    // and we're done
    ConnectionImpl::pushSetup(Status::OK());
    ASSERT_EQ(ConnectionImpl::setupQueueDepth(), 0u);
    ams[4].wait();
    firstNBound(5);

    for (auto& conn : conns) {
        doneWith(conn);
    }
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
    auto pool = makePool(options);

    auto now = Date_t::now();

    PoolImpl::setNow(now);

    ConnectionPool::ConnectionHandle conn1;
    ConnectionPool::ConnectionHandle conn2;
    ConnectionPool::ConnectionHandle conn3;

    bool reachedA = false;
    bool reachedB = false;
    bool reachedC = false;

    // Grab one connection without returning it
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(1000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  ASSERT(swConn.isOK());

                                  conn1 = std::move(swConn.getValue());
                              });
                          });

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
    });

    // Verify that two setups were invoked, even without two requests (the
    // minConnections == 2)
    ASSERT(reachedA);
    ASSERT(reachedB);
    ASSERT(!reachedC);

    // Two more get's without returns
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(2000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  ASSERT(swConn.isOK());

                                  conn2 = std::move(swConn.getValue());
                              });
                          });
    });
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(3000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  ASSERT(swConn.isOK());

                                  conn3 = std::move(swConn.getValue());
                              });
                          });
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

    PoolImpl::setNow(now + Milliseconds(2));
    doneWith(conn2);

    PoolImpl::setNow(now + Milliseconds(3));
    doneWith(conn3);

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
    auto pool = makePool(options);

    auto now = Date_t::now();

    PoolImpl::setNow(now);

    size_t connId = 0;

    bool reachedA = false;
    // Grab 1 connection and return it
    ConnectionImpl::pushSetup(Status::OK());
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  connId = verifyAndGetId(swConn);
                                  reachedA = true;
                                  doneWith(swConn.getValue());
                              });
                          });
    });
    ASSERT(reachedA);

    // Jump pass the hostTimeout
    PoolImpl::setNow(now + Milliseconds(1000));

    bool reachedB = false;

    // Verify that a new connection was spawned
    ConnectionImpl::pushSetup(Status::OK());
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  ASSERT_NE(connId, verifyAndGetId(swConn));
                                  reachedB = true;
                                  doneWith(swConn.getValue());
                              });
                          });
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
    auto pool = makePool(options);

    auto now = Date_t::now();

    PoolImpl::setNow(now);

    size_t connId = 0;

    bool reachedA = false;

    // Grab and return
    ConnectionImpl::pushSetup(Status::OK());
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  connId = verifyAndGetId(swConn);
                                  reachedA = true;
                                  doneWith(swConn.getValue());
                              });
                          });
    });
    ASSERT(reachedA);

    // Jump almost up to the hostTimeout
    PoolImpl::setNow(now + Milliseconds(999));

    bool reachedB = false;
    // Same connection
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  ASSERT_EQ(connId, verifyAndGetId(swConn));
                                  reachedB = true;
                                  doneWith(swConn.getValue());
                              });
                          });
    });
    ASSERT(reachedB);

    // Now our timeout should be 1999 ms from 'now' instead of 1000 ms
    // if we do another 'get' we should still get the original connection
    PoolImpl::setNow(now + Milliseconds(1500));
    bool reachedB2 = false;
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  ASSERT_EQ(connId, verifyAndGetId(swConn));
                                  reachedB2 = true;
                                  doneWith(swConn.getValue());
                              });
                          });
    });
    ASSERT(reachedB2);

    // We should time out when we get to 'now' + 2500 ms
    PoolImpl::setNow(now + Milliseconds(2500));

    bool reachedC = false;
    // Different id
    ConnectionImpl::pushSetup(Status::OK());
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  ASSERT_NE(connId, verifyAndGetId(swConn));
                                  reachedC = true;
                                  doneWith(swConn.getValue());
                              });
                          });
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
    auto pool = makePool(options);

    auto now = Date_t::now();

    PoolImpl::setNow(now);

    ConnectionPool::ConnectionHandle conn1;
    size_t conn1Id = 0;
    size_t conn2Id = 0;

    // save 1 connection
    ConnectionImpl::pushSetup(Status::OK());
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  conn1Id = verifyAndGetId(swConn);
                                  conn1 = std::move(swConn.getValue());
                              });
                          });
    });
    ASSERT(conn1Id);

    // return the second
    ConnectionImpl::pushSetup(Status::OK());
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  conn2Id = verifyAndGetId(swConn);
                                  doneWith(swConn.getValue());
                              });
                          });
    });
    ASSERT(conn2Id);

    // hostTimeout has passed
    PoolImpl::setNow(now + Milliseconds(1000));

    bool reachedA = false;

    // conn 2 is still there
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  ASSERT_EQ(conn2Id, verifyAndGetId(swConn));
                                  reachedA = true;
                                  doneWith(swConn.getValue());
                              });
                          });
    });
    ASSERT(reachedA);

    // return conn 1
    doneWith(conn1);

    // expire the pool
    PoolImpl::setNow(now + Milliseconds(2000));

    bool reachedB = false;

    // make sure that this is a new id
    ConnectionImpl::pushSetup(Status::OK());
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  ASSERT_NE(conn1Id, verifyAndGetId(swConn));
                                  ASSERT_NE(conn2Id, verifyAndGetId(swConn));
                                  reachedB = true;
                                  doneWith(swConn.getValue());
                              });
                          });
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
    auto pool = makePool(options);

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    // Grab the first connection id
    size_t conn1Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  conn1Id = verifyAndGetId(swConn);
                                  doneWith(swConn.getValue());
                              });
                          });
    });
    ASSERT(conn1Id);

    // Grab it and this time keep it out of the pool
    ConnectionPool::ConnectionHandle handle;
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  ASSERT_EQ(verifyAndGetId(swConn), conn1Id);
                                  handle = std::move(swConn.getValue());
                              });
                          });
    });
    ASSERT(handle);

    bool reachedA = false;

    // Queue up a request. This won't fire until we drop connections, then it
    // will fail.
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  ASSERT(!swConn.isOK());
                                  reachedA = true;
                              });
                          });
        ASSERT(!reachedA);

        // fails the previous get
        pool->dropConnections(HostAndPort());
    });
    ASSERT(reachedA);

    // return the connection
    doneWith(handle);

    // Make sure that a new connection request properly disposed of the gen1
    // connection
    size_t conn2Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  conn2Id = verifyAndGetId(swConn);
                                  ASSERT_NE(conn2Id, conn1Id);
                                  doneWith(swConn.getValue());
                              });
                          });
    });
    ASSERT(conn2Id);

    // Push conn2 into refresh
    PoolImpl::setNow(now + Milliseconds(1500));

    // drop the connections
    pool->dropConnections(HostAndPort());

    // refresh still pending
    PoolImpl::setNow(now + Milliseconds(2500));

    // Verify that a new connection came out, despite the gen2 connection still
    // being pending
    bool reachedB = false;
    ConnectionImpl::pushSetup(Status::OK());
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  ASSERT_NE(verifyAndGetId(swConn), conn2Id);
                                  reachedB = true;
                                  doneWith(swConn.getValue());
                              });
                          });
    });
    ASSERT(reachedB);
}

TEST_F(ConnectionPoolTest, DropAllConnectionsWithKeepOpen) {
    ConnectionPool::Options options;

    options.maxConnections = 2;
    options.refreshRequirement = Seconds(1);
    options.refreshTimeout = Seconds(2);
    auto pool = makePool(options);

    PoolImpl::setNow(Date_t::now());

    auto requestConnection = [&](HostAndPort hp,
                                 CancellationToken token = CancellationToken::uncancelable()) {
        return getFromPool(hp, transport::kGlobalSSLMode, Seconds{5}, token);
    };

    HostAndPort hostKeepOpen("a");
    HostAndPort hostNoKeepOpen("b");

    // Initialize pools for two hosts by getting connections from them so we can control
    // keepOpen for those pools individually. Keep a connection checked out from each one so
    // that the next request won't immediately ready a connection.
    ConnectionImpl::pushSetup(Status::OK());
    auto ch1 = requestConnection(hostKeepOpen).get();
    ConnectionImpl::pushSetup(Status::OK());
    auto ch2 = requestConnection(hostNoKeepOpen).get();

    ScopeGuard cleanupHandles([&] {
        if (ch1)
            doneWith(ch1);
        if (ch2)
            doneWith(ch2);
    });

    ASSERT(ch1);
    ASSERT(ch2);

    // Pools for hosts have been created, set keepOpen accordingly.
    pool->setKeepOpen(hostKeepOpen, true);
    pool->setKeepOpen(hostNoKeepOpen, false);

    // Request a connection from the non-keepOpen pool. We should see that fail once we drop
    // connections.
    auto noKeepOpenConnFuture = requestConnection(hostNoKeepOpen);

    // Request a connection from the keepOpen pool. This one should not be dropped. We'll later
    // cancel it instead of messing with pushSetup (which at this point would fulfill the request
    // above that we want to see get dropped).
    CancellationSource keepOpenCancelSource;
    auto keepOpenConnFuture = requestConnection(hostKeepOpen, keepOpenCancelSource.token());

    // Dropping connections should fail the pending request on the non-keepOpen pool.
    ASSERT_FALSE(noKeepOpenConnFuture.isReady());
    pool->dropConnections();
    {
        auto swConn = std::move(noKeepOpenConnFuture).getNoThrow();
        ASSERT_EQ(swConn.getStatus().code(), ErrorCodes::PooledConnectionsDropped);
    }

    // Cancel the keepOpen pool request.
    ASSERT_FALSE(keepOpenConnFuture.isReady());
    keepOpenCancelSource.cancel();
    {
        auto swConn = std::move(keepOpenConnFuture).getNoThrow();
        ASSERT_EQ(swConn.getStatus().code(), ErrorCodes::CallbackCanceled);
    }
}

/**
 * Verify that setup timeouts time out other pending requests. This is in adherence with
 * the SDAM specification which states that timeouts during setup should mark the timed
 * out host as Unknown. Therefore, pending connections may be dropped in this layer as
 * a reaction to setup timeout.
 */
TEST_F(ConnectionPoolTest, SetupTimeoutsFailOtherPendingRequests) {
    ConnectionPool::Options options;

    options.maxConnections = 1;
    options.refreshTimeout = Seconds(2);
    auto pool = makePool(options);

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    boost::optional<StatusWith<ConnectionPool::ConnectionHandle>> conn1;
    pool->get_forTest(
        HostAndPort(), Seconds(10), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            conn1 = std::move(swConn);
        });

    // Initially we haven't called our callback.
    ASSERT(!conn1);

    PoolImpl::setNow(now + Seconds(1));

    // Still haven't fired on conn1.
    ASSERT(!conn1);

    boost::optional<StatusWith<ConnectionPool::ConnectionHandle>> conn2;
    // Get conn2 (which should have an extra second before the timeout).
    pool->get_forTest(
        HostAndPort(), Seconds(10), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            conn2 = std::move(swConn);
        });

    PoolImpl::setNow(now + Seconds(2));

    ASSERT(conn1);
    ASSERT(!conn1->isOK());
    ASSERT_EQ(conn1->getStatus(), ErrorCodes::HostUnreachable);
    ASSERT(conn2);
    ASSERT(!conn2->isOK());
    // Pending connection fails with the same timeout status.
    ASSERT_EQ(conn2->getStatus(), ErrorCodes::HostUnreachable);
}

/**
 * Verify that timeouts during refresh time out other pending requests.
 */
TEST_F(ConnectionPoolTest, RefreshTimeoutsFailPendingRequests) {
    ConnectionPool::Options options;

    options.maxConnections = 1;
    options.refreshTimeout = Seconds(2);
    options.refreshRequirement = Seconds(3);
    auto pool = makePool(options);

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    // Successfully get a new connection
    size_t conn1Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(
        HostAndPort(), Seconds(1), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            conn1Id = verifyAndGetId(swConn);
            doneWith(swConn.getValue());
        });
    ASSERT(conn1Id);

    // Force it into refresh
    PoolImpl::setNow(now + Seconds(3));

    boost::optional<StatusWith<ConnectionPool::ConnectionHandle>> conn1;
    pool->get_forTest(
        HostAndPort(), Seconds(10), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            conn1 = std::move(swConn);
        });

    // initially we haven't called our callback
    ASSERT(!conn1);

    // 1 second later we've triggered a refresh and still haven't called the callback
    PoolImpl::setNow(now + Seconds(4));
    ASSERT(!conn1);

    // Get conn2 (which should have an extra second before the timeout)
    pool->get_forTest(
        HostAndPort(), Seconds(10), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            ASSERT_EQ(swConn.getStatus(), ErrorCodes::HostUnreachable);
        });

    PoolImpl::setNow(now + Seconds(5));

    ASSERT(conn1);
    ASSERT(!conn1->isOK());
    ASSERT_EQ(conn1->getStatus(), ErrorCodes::HostUnreachable);
}

template <typename Ptr>
void ConnectionPoolTest::dropConnectionsTest(std::shared_ptr<ConnectionPool> const& pool, Ptr t) {
    auto now = Date_t::now();
    PoolImpl::setNow(now);

    HostAndPort hap1("a");
    HostAndPort hap2("b");
    HostAndPort hap3("c");
    HostAndPort hap4("d");

    // Successfully get connections to two hosts
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(hap1, Seconds(1), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
        doneWith(swConn.getValue());
    });

    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(hap2, Seconds(1), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
        doneWith(swConn.getValue());
    });

    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(hap3, Seconds(1), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
        doneWith(swConn.getValue());
    });

    ASSERT_EQ(1ul, pool->getNumConnectionsPerHost(hap1));
    ASSERT_EQ(1ul, pool->getNumConnectionsPerHost(hap2));
    ASSERT_EQ(1ul, pool->getNumConnectionsPerHost(hap3));

    t->dropConnections();

    ASSERT_EQ(1ul, pool->getNumConnectionsPerHost(hap1));
    ASSERT_EQ(1ul, pool->getNumConnectionsPerHost(hap2));
    ASSERT_EQ(1ul, pool->getNumConnectionsPerHost(hap3));

    t->setKeepOpen(hap1, true);

    t->setKeepOpen(hap2, true);

    t->setKeepOpen(hap3, false);

    t->dropConnections();

    ASSERT_EQ(1ul, pool->getNumConnectionsPerHost(hap1));
    ASSERT_EQ(1ul, pool->getNumConnectionsPerHost(hap2));
    ASSERT_EQ(0ul, pool->getNumConnectionsPerHost(hap3));

    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(hap4, Seconds(1), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
        doneWith(swConn.getValue());
    });

    // drop connections by hostAndPort
    t->dropConnections(hap1);

    ASSERT_EQ(0ul, pool->getNumConnectionsPerHost(hap1));
    ASSERT_EQ(1ul, pool->getNumConnectionsPerHost(hap2));
    ASSERT_EQ(0ul, pool->getNumConnectionsPerHost(hap3));
    ASSERT_EQ(1ul, pool->getNumConnectionsPerHost(hap4));
}

TEST_F(ConnectionPoolTest, DropConnections) {
    ConnectionPool::Options options;
    options.minConnections = 0;
    auto pool = makePool(options);

    dropConnectionsTest(pool, pool);
}

TEST_F(ConnectionPoolTest, DropConnectionsInMultipleViaManager) {
    EgressConnectionCloserManager manager;
    ConnectionPool::Options options;
    options.minConnections = 0;
    options.egressConnectionCloserManager = &manager;
    auto pool = makePool(options);

    dropConnectionsTest(pool, &manager);
}

TEST_F(ConnectionPoolTest, AsyncGet) {
    ConnectionPool::Options options;
    options.maxConnections = 1;
    auto pool = makePool(options);

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    // Make our initial connection, use and return it
    {
        size_t connId = 0;

        // no connections in the pool, our future is not satisfied
        auto connFuture = getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds{1});
        ASSERT_FALSE(connFuture.isReady());

        // Successfully get a new connection
        ConnectionImpl::pushSetup(Status::OK());

        // Future should be ready now
        ASSERT_TRUE(connFuture.isReady());
        auto conn = std::move(connFuture).get();
        connId = getId(conn);
        doneWith(conn);
        ASSERT(connId);
    }

    // There is one connection in the pool:
    // * The first get should resolve immediately
    // * The second get should should be queued
    // * The eventual third should be queued before the second
    {
        size_t connId1 = 0;
        size_t connId2 = 0;
        size_t connId3 = 0;

        auto connFuture1 = getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds{1});
        auto connFuture2 = getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds{10});

        // The first future should be immediately ready. The second should be in the queue.
        ASSERT_TRUE(connFuture1.isReady());
        ASSERT_FALSE(connFuture2.isReady());

        // Resolve the first future to return the connection and continue on to the second.
        decltype(connFuture1) connFuture3;
        auto conn1 = std::move(connFuture1).get();

        // Grab our third future while our first one is being fulfilled
        connFuture3 = getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds{1});

        connId1 = getId(conn1);
        doneWith(conn1);
        ASSERT(connId1);

        // Since the third future has a smaller timeout than the second,
        // it should take priority over the second
        ASSERT_TRUE(connFuture3.isReady());
        ASSERT_FALSE(connFuture2.isReady());

        // Resolve the third future. This should trigger the second future
        auto conn3 = std::move(connFuture3).get();

        // We've run before the second future
        ASSERT_FALSE(connFuture2.isReady());

        connId3 = getId(conn3);
        doneWith(conn3);

        // The second future is now finally ready
        ASSERT_TRUE(connFuture2.isReady());
        auto conn2 = std::move(connFuture2).get();
        connId2 = getId(conn2);
        doneWith(conn2);

        ASSERT_EQ(connId1, connId2);
        ASSERT_EQ(connId2, connId3);
    }
}

TEST_F(ConnectionPoolTest, NegativeTimeout) {
    ConnectionPool::Options options;
    options.maxConnections = 1;
    auto pool = makePool(options);

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(1000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() { ASSERT(!swConn.isOK()); });
                          });
        // Advance our timer past the request timeout, so that the resultant internal
        // timeout in updateEventTimer is negative, and make sure we don't trip any
        // assertions.
        PoolImpl::setNow(now + Milliseconds(2000));

        ConnectionImpl::pushSetup(Status::OK());
    });
}

TEST_F(ConnectionPoolTest, ReturnAfterShutdown) {
    auto pool = makePool();

    // Grab a connection and hold it to end of scope
    auto connFuture = getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds(1));
    ConnectionImpl::pushSetup(Status::OK());
    auto conn = std::move(connFuture).get();
    doneWith(conn);

    pool->shutdown();
}

TEST_F(ConnectionPoolTest, TotalConnUseTimeIncreasedForCheckedOutConnection) {
    constexpr Milliseconds checkOutLength = Milliseconds(10);
    auto pool = makePool();

    ConnectionPoolStats initialStats;
    pool->appendConnectionStats(&initialStats);

    auto startTimePoint = Date_t::now();
    auto endTimePoint = startTimePoint + checkOutLength;
    PoolImpl::setNow(startTimePoint);

    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          PoolImpl::setNow(endTimePoint);
                          doneWith(swConn.getValue());
                      });

    ConnectionPoolStats finalStats;
    pool->appendConnectionStats(&finalStats);

    auto totalTimeUsageDelta = finalStats.totalConnUsageTime - initialStats.totalConnUsageTime;
    ASSERT_GREATER_THAN_OR_EQUALS(totalTimeUsageDelta, checkOutLength);
}

TEST_F(ConnectionPoolTest, OverlappingCheckoutsAdditivelyContributeToTotalUsageTime) {
    constexpr Milliseconds checkOutLength = Milliseconds(10);
    auto pool = makePool();

    ConnectionPoolStats initialStats;
    pool->appendConnectionStats(&initialStats);

    auto startTimePoint = Date_t::now();
    auto endTimePoint = startTimePoint + checkOutLength;
    PoolImpl::setNow(startTimePoint);

    // Check out multiple connections.
    constexpr int numConnections = 2;
    std::vector<ConnectionPool::ConnectionHandle> connections;
    // Ensure that no matter how we leave the test, we mark any
    // checked out connections as OK before implicity returning them
    // to the pool by destroying the 'connections' vector. Otherwise,
    // this test would cause an invariant failure instead of a normal
    // test failure if it fails, which would be confusing.
    ScopeGuard guard([&] {
        while (!connections.empty()) {
            try {
                ConnectionPool::ConnectionHandle conn = std::move(connections.back());
                connections.pop_back();
                doneWith(conn);
            } catch (...) {
            }
        }
    });

    for (int i = 0; i < numConnections; ++i) {
        ConnectionImpl::pushSetup(Status::OK());
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              ASSERT(swConn.isOK());
                              connections.push_back(std::move(swConn.getValue()));
                          });
    }
    ASSERT_EQ(connections.size(), numConnections);
    // Advance the time and return the connections.
    PoolImpl::setNow(endTimePoint);
    while (!connections.empty()) {
        try {
            ConnectionPool::ConnectionHandle conn = std::move(connections.back());
            ASSERT(conn);
            connections.pop_back();
            doneWith(conn);
        } catch (...) {
        }
    }
    guard.dismiss();

    ConnectionPoolStats finalStats;
    pool->appendConnectionStats(&finalStats);

    auto totalTimeUsageDelta = finalStats.totalConnUsageTime - initialStats.totalConnUsageTime;
    // Since each connection was used for checkOutLength, the total usage time should be >= the
    // product.
    ASSERT_GREATER_THAN_OR_EQUALS(totalTimeUsageDelta, checkOutLength * numConnections);
}

TEST_F(ConnectionPoolTest, LeasedConnectionsDontCountTowardsUsageTime) {
    constexpr Milliseconds checkOutLength = Milliseconds(10);
    auto pool = makePool();

    ConnectionPoolStats initialStats;
    pool->appendConnectionStats(&initialStats);

    auto startTimePoint = Date_t::now();
    auto endTimePoint = startTimePoint + checkOutLength;
    PoolImpl::setNow(startTimePoint);

    ConnectionImpl::pushSetup(Status::OK());
    pool->lease_forTest(HostAndPort(),
                        Milliseconds(5000),
                        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                            PoolImpl::setNow(endTimePoint);
                            doneWith(swConn.getValue());
                        });

    ConnectionPoolStats finalStats;
    pool->appendConnectionStats(&finalStats);

    auto totalTimeUsageDelta = finalStats.totalConnUsageTime - initialStats.totalConnUsageTime;
    ASSERT_EQ(totalTimeUsageDelta, Milliseconds(0));
}

TEST_F(ConnectionPoolTest, LeasedConnectionsDontInterfereWithOrdinaryCheckoutUsageTime) {
    constexpr Milliseconds checkOutLength = Milliseconds(10);
    auto pool = makePool();

    ConnectionPoolStats initialStats;
    pool->appendConnectionStats(&initialStats);

    auto startTimePoint = Date_t::now();
    auto endTimePoint = startTimePoint + checkOutLength;
    PoolImpl::setNow(startTimePoint);

    ConnectionImpl::pushSetup(Status::OK());

    // Checkout one connection and lease one connection.
    ConnectionPool::ConnectionHandle normal;
    ConnectionPool::ConnectionHandle leased;
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          normal = std::move(swConn.getValue());
                      });
    pool->lease_forTest(HostAndPort(),
                        Milliseconds(5000),
                        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                            leased = std::move(swConn.getValue());
                        });

    // Advance the time and return the connections.
    PoolImpl::setNow(endTimePoint);
    doneWith(normal);
    doneWith(leased);

    ConnectionPoolStats finalStats;
    pool->appendConnectionStats(&finalStats);

    auto totalTimeUsageDelta = finalStats.totalConnUsageTime - initialStats.totalConnUsageTime;
    // Should only include usage time from the checkout, not the lease
    ASSERT_GREATER_THAN_OR_EQUALS(totalTimeUsageDelta, checkOutLength);
    ASSERT_LESS_THAN(totalTimeUsageDelta, checkOutLength * 2);
}

TEST_F(ConnectionPoolTest, CancelGetBeforeCallDoesntPullConnection) {
    CancellationSource source;
    auto pool = makePool();

    source.cancel();
    auto connFuture =
        getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds(1), source.token());
    ASSERT_TRUE(connFuture.isReady());
    ASSERT_THROWS_CODE(connFuture.get(), DBException, ErrorCodes::CallbackCanceled);
}

TEST_F(ConnectionPoolTest, CancelGetEarlyDoesntPullConnection) {
    CancellationSource source;
    auto pool = makePool();

    auto connFuture =
        getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds(1), source.token());
    ASSERT_FALSE(connFuture.isReady());

    source.cancel();
    ASSERT_TRUE(connFuture.isReady());
    ASSERT_THROWS_CODE(connFuture.get(), DBException, ErrorCodes::CallbackCanceled);
}

TEST_F(ConnectionPoolTest, CancelGetEarlyWithReadyConnectionDoesntPullConnection) {
    CancellationSource source;
    auto pool = makePool();

    // Make a connection and return it so that the next request is immediately fulfillable.
    ConnectionImpl::pushSetup(Status::OK());
    {
        auto connFuture = getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds(1));
        ASSERT_TRUE(connFuture.isReady());
        doneWith(connFuture.get());
    }

    ASSERT_EQ(pool->getNumConnectionsPerHost(HostAndPort()), 1);

    source.cancel();
    {
        auto connFuture =
            getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds(1), source.token());
        ASSERT_TRUE(connFuture.isReady());
        ASSERT_THROWS_CODE(connFuture.get(), DBException, ErrorCodes::CallbackCanceled);
    }
}

TEST_F(ConnectionPoolTest, CancelGetLatePullsConnection) {
    CancellationSource source;
    auto pool = makePool();

    auto connFuture =
        getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds(1), source.token());
    ASSERT_FALSE(connFuture.isReady());

    ConnectionImpl::pushSetup(Status::OK());
    ASSERT_TRUE(connFuture.isReady());

    source.cancel();
    doneWith(connFuture.get());
}

TEST_F(ConnectionPoolTest, CancelGetAfterDestruction) {
    CancellationSource source;
    auto pool = makePool();

    auto connFuture =
        getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds(1), source.token());
    ASSERT_FALSE(connFuture.isReady());

    ConnectionImpl::pushSetup(Status::OK());
    ASSERT_TRUE(connFuture.isReady());
    doneWith(connFuture.get());

    pool->shutdown();
    pool = {};
    dropPool();

    source.cancel();
}

TEST_F(ConnectionPoolTest, DismissBeforeCancelGet) {
    CancellationSource source;
    auto pool = makePool();

    auto connFuture =
        getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds(1), source.token());
    ASSERT_FALSE(connFuture.isReady());

    source = {};
    ASSERT_FALSE(connFuture.isReady());

    ConnectionImpl::pushSetup(Status::OK());
    ASSERT_TRUE(connFuture.isReady());
    doneWith(connFuture.get());
}

TEST_F(ConnectionPoolTest, EnsureReasonIsLogged) {
    // Bumping up the log severity for this unit test to catch all logs.
    unittest::MinimumLoggedSeverityGuard logSeverityGuardNetwork{
        logv2::LogComponent::kConnectionPool, logv2::LogSeverity::Debug(5)};

    ConnectionPool::Options options;
    options.minConnections = 0;
    auto pool = makePool(options);

    unittest::LogCaptureGuard logs;

    HostAndPort hap("a");

    // Successfully get connection to host.
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(
        hap, Milliseconds(0), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            doneWith(swConn.getValue());
        });

    std::string reason = "TEST: Ensuring reason is logged";

    // Check number of connections before and after dropping connection to host.
    ASSERT_EQ(1ul, pool->getNumConnectionsPerHost(hap));
    pool->dropConnections(hap, Status(ErrorCodes::PooledConnectionsDropped, reason));
    ASSERT_EQ(0ul, pool->getNumConnectionsPerHost(hap));

    logs.stop();

    // Check the BSON format for the specific log message.
    auto msgCounter = logs.countBSONContainingSubset(
        BSON("attr" << BSON("error" << "PooledConnectionsDropped: " + reason)));
    ASSERT_EQ(1ul, msgCounter);
}

}  // namespace connection_pool_test_details
}  // namespace executor
}  // namespace mongo
