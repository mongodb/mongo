// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/connection_pool.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/executor/connection_pool_controllers.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/connection_pool_test_fixture.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/executor_test_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"

#include <algorithm>
#include <array>
#include <memory>
#include <random>
#include <ratio>
#include <set>
#include <stack>
#include <string_view>
#include <tuple>
#include <type_traits>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {
namespace executor {
namespace connection_pool_test_details {
using namespace std::literals::string_view_literals;

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

    ExecutorFuture<void> doneWithAsync(ConnectionPool::ConnectionHandle& conn) {
        dynamic_cast<ConnectionImpl*>(conn.get())->indicateSuccess();

        return ExecutorFuture(_executor).then([conn = std::move(conn)]() {});
    }

    void doneWith(ConnectionPool::ConnectionHandle& conn) {
        doneWithAsync(conn).getAsync([](auto) {});
    }

    void doneWithError(ConnectionPool::ConnectionHandle& conn, Status error) {
        dynamic_cast<ConnectionImpl*>(conn.get())->indicateFailure(error);

        ExecutorFuture(_executor).getAsync([conn = std::move(conn)](auto) {});
    }

    /**
     * Destroys a connection handle without indicating success or failure by scheduling its
     * destruction onto the executor. Use this instead of destroying the handle directly to avoid
     * deadlocking with the inline test executor.
     */
    void dropHandle(ConnectionPool::ConnectionHandle& conn) {
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

    ConnectionPoolStats getStats(std::shared_ptr<ConnectionPool> pool) {
        ConnectionPoolStats stats;
        pool->appendConnectionStats(&stats);
        return stats;
    }

    std::tuple<std::shared_ptr<ConnectionPool>, std::vector<ConnectionPool::ConnectionHandle>>
    setupConnectionPool(size_t inUseConnections,
                        size_t availableConnections,
                        size_t settingUpConnections,
                        std::function<void(ConnectionPool::Options& options)> updateOptionsFn,
                        ConnectionPool::GetConnectionCallback getConnectionToTriggerSetupCb,
                        Milliseconds acquisitionTimeout = Seconds{10}) {
        const auto totalConnections =
            availableConnections + inUseConnections + settingUpConnections;

        // In order to reach the requested pool status, we need to set the following options to the
        // total amount of requested connections.
        ConnectionPool::Options options;
        options.maxConnections = totalConnections;
        options.minConnections = totalConnections;
        options.maxConnecting = totalConnections;

        updateOptionsFn(options);

        ASSERT_EQ(totalConnections, options.maxConnections)
            << "maxConnections can't be updated through the `updateOptionsFn`";
        ASSERT_EQ(totalConnections, options.minConnections)
            << "minConnections can't be updated through the `updateOptionsFn`";
        ASSERT_EQ(totalConnections, options.maxConnecting)
            << "maxConnecting can't be updated through the `updateOptionsFn`";

        if (availableConnections != 0) {
            // In order to keep the returned connections in the available pool, we need to ensure
            // that returning a connection won't trigger a refresh.
            ASSERT_TRUE(options.refreshRequirement > Milliseconds{0});
        }

        auto pool = makePool(options);

        std::vector<boost::optional<StatusWith<ConnectionPool::ConnectionHandle>>> inUseConns;

        // Initiate the requested checked out connections.
        for (size_t i = 0; i < inUseConnections; ++i) {
            inUseConns.emplace_back(boost::none);

            ConnectionImpl::pushSetup(Status::OK());
            pool->get_forTest(HostAndPort(),
                              acquisitionTimeout,
                              [&, connId = i](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                                  inUseConns[connId] = std::move(swConn);
                              });
            ASSERT(inUseConns[i].has_value());
            ASSERT(inUseConns[i]->isOK());
        }

        // To create available connections, we'll first use them and return them back to the
        // pool.
        for (size_t i = 0; i < availableConnections; ++i) {
            boost::optional<StatusWith<ConnectionPool::ConnectionHandle>> conn;
            ConnectionImpl::pushSetup(Status::OK());
            pool->get_forTest(HostAndPort(),
                              acquisitionTimeout,
                              [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                                  conn = std::move(swConn);
                              });
            ASSERT(conn);
            ASSERT(conn->isOK());

            doneWith(conn->getValue());
        }

        // If we have already requested a connection, we'll automatically have the requested
        // `setupConnections`. Otherwise, we'll have to trigger a connections spawn by requesting a
        // new connection.
        // In the last case, the user will have to manage the connection retrieval from outside this
        // function.
        if (availableConnections + inUseConnections == 0 && settingUpConnections > 0) {
            pool->get_forTest(
                HostAndPort(), acquisitionTimeout, std::move(getConnectionToTriggerSetupCb));
        }

        auto connStats = getStats(pool);
        ASSERT_EQ(availableConnections, connStats.totalAvailable);
        ASSERT_EQ(inUseConnections, connStats.totalInUse);
        ASSERT_EQ(settingUpConnections, connStats.totalRefreshing);

        // Simplify the inUseConns vector to return it to the user.
        std::vector<ConnectionPool::ConnectionHandle> inUseConnsToReturn;
        inUseConnsToReturn.reserve(inUseConns.size());
        std::transform(inUseConns.begin(),
                       inUseConns.end(),
                       std::back_inserter(inUseConnsToReturn),
                       [](auto&& conn) { return std::move(conn->getValue()); });

        return std::make_tuple(pool, std::move(inUseConnsToReturn));
    };

    std::shared_ptr<OutOfLineExecutor> _executor = InlineQueuedCountingExecutor::make();

private:
    std::shared_ptr<ConnectionPool> _pool;
    unittest::MinimumLoggedSeverityGuard logSeverityGuardNetwork{
        logv2::LogComponent::kConnectionPool, logv2::LogSeverity::Debug(5)};
};

// Fixtures are listed in execution-flow order. Tests within each fixture run in definition order.
class ConnectionPoolCheckoutTest : public ConnectionPoolTest {};
class ConnectionPoolQueuingTest : public ConnectionPoolTest {};
class ConnectionPoolSpawningTest : public ConnectionPoolTest {};
class ConnectionPoolSetupTest : public ConnectionPoolTest {};
class ConnectionPoolReturnAndRefreshTest : public ConnectionPoolTest {};
class ConnectionPoolLeasingTest : public ConnectionPoolTest {};
class ConnectionPoolFailureTest : public ConnectionPoolTest {};
class ConnectionPoolExpiryTest : public ConnectionPoolTest {};
class ConnectionPoolDropTest : public ConnectionPoolTest {};
class ConnectionPoolCancellationTest : public ConnectionPoolTest {};
class ConnectionPoolShutdownTest : public ConnectionPoolTest {};
class ConnectionPoolMetricsTest : public ConnectionPoolTest {};

// This fixture is for testing the DynamicLimitController, which is a controller that adjusts
// the max connections and pending connections limits based on user-provided functions. Since the
// controller doesn't have complex interactions with the pool, we can test it in a
// separate fixture with more focused tests that don't require the full range of pool
// operations to be tested.
class DynamicLimitControllerTest : public ConnectionPoolTest {
protected:
    using ControllerPtr = std::shared_ptr<DynamicLimitController>;

    std::tuple<std::shared_ptr<ConnectionPool>, ControllerPtr> makeDynamicController(
        size_t min = 1,
        size_t max = 10,
        ConnectionPool::Options opts = {},
        std::string_view name = "dynamic limit controller") {
        return makeDynamicController(
            [min] { return min; }, [max] { return max; }, std::move(opts), std::move(name));
    }

    std::tuple<std::shared_ptr<ConnectionPool>, ControllerPtr> makeDynamicController(
        std::function<size_t()> minLoader,
        std::function<size_t()> maxLoader,
        ConnectionPool::Options opts = {},
        std::string_view name = "dynamic limit controller") {
        auto controller = std::make_shared<DynamicLimitController>(
            std::move(minLoader), std::move(maxLoader), std::move(name));
        auto pool = makePool(opts);
        controller->init(pool.get());
        return {std::move(pool), std::move(controller)};
    }

    ConnectionPool::ConnectionControls addHostAndUpdate(
        const ControllerPtr& controller,
        ConnectionPool::PoolId id,
        const HostAndPort& host,
        const ConnectionPool::PoolMetrics& metrics) {
        controller->addHost(id, host);
        controller->updateHost(id, metrics);
        return controller->getControls(id);
    }

    ConnectionPool::ConnectionControls updateHostAndGetControls(
        const ControllerPtr& controller,
        ConnectionPool::PoolId id,
        const ConnectionPool::PoolMetrics& metrics) {
        controller->updateHost(id, metrics);
        return controller->getControls(id);
    }
};

template <typename Traits>
class ConnectionPoolLimitControllerTest : public ConnectionPoolTest {
protected:
    using ControllerPtr = std::shared_ptr<ConnectionPool::ControllerInterface>;

    std::tuple<std::shared_ptr<ConnectionPool>, ControllerPtr> setupLimitController(
        ConnectionPool::Options opts = {}) {
        static_assert(std::is_convertible_v<decltype(Traits::makeLimitController(
                                                std::declval<const ConnectionPool::Options&>())),
                                            ControllerPtr>,
                      "Traits::makeLimitController(const ConnectionPool::Options&) must return a "
                      "shared_ptr<ConnectionPool::ControllerInterface>");

        auto controller = Traits::makeLimitController(opts);
        opts.controllerFactory = [controller]() {
            return controller;
        };
        auto pool = makePool(opts);
        return std::make_tuple(std::move(pool), std::move(controller));
    }
};

// The following traits and test suite are for testing the behavior of the ConnectionPool when using
// different types of controllers. The tests themselves are defined in the
// ConnectionPoolLimitControllerTest fixture, and the traits are used to instantiate the test suite
// with different controller types. This allows us to run the same set of tests against both the
// default LimitController and the DynamicLimitController without having to duplicate the test code.

struct LimitControllerTrait {
    static std::shared_ptr<ConnectionPool::ControllerInterface> makeLimitController(
        const ConnectionPool::Options&) {
        return ConnectionPool::makeLimitController();
    }
};

struct DynamicLimitControllerTrait {
    static std::shared_ptr<ConnectionPool::ControllerInterface> makeLimitController(
        const ConnectionPool::Options& opts) {
        return std::make_shared<DynamicLimitController>([min = opts.minConnections] { return min; },
                                                        [max = opts.maxConnections] { return max; },
                                                        "dynamic limit controller");
    }
};

using ConnectionPoolLimitControllerTestTypes =
    ::testing::Types<LimitControllerTrait, DynamicLimitControllerTrait>;
TYPED_TEST_SUITE(ConnectionPoolLimitControllerTest, ConnectionPoolLimitControllerTestTypes);

/**
 * Verify that a request is rejected immediately when the pending request queue is at capacity.
 */
TEST_F(ConnectionPoolQueuingTest, RequestRejectedWhenQueueDepthExceeded) {
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
 * Verify that a request is rejected immediately before it enters the queue, without needing the
 * queue to be at capacity.
 */
TEST_F(ConnectionPoolQueuingTest, RequestRejectedBeforeQueuing) {
    auto pool = makePool();
    FailPointEnableBlock fpb("connectionPoolRejectsConnectionRequests");
    auto fut = getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds(1));
    ASSERT_TRUE(fut.isReady());
    ASSERT_THROWS_CODE(
        std::move(fut).get(), DBException, ErrorCodes::PooledConnectionAcquisitionRejected);
}

/**
 * Verify that connection stats (totalCreated, per-host created) are accumulated correctly across
 * pool drops and reconnections, and that dropping connections does not reset the created count.
 */
TEST_F(ConnectionPoolMetricsTest, ConnectionStatsAreReportedCorrectly) {
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
TEST_F(ConnectionPoolCheckoutTest, ReturnedConnectionIsReusedOnNextCheckout) {
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
TEST_F(ConnectionPoolCheckoutTest, ConnectionsAreAcquiredInMRUOrder) {
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
 * Verify that recently used connections are not purged, while connections not used recently are.
 */
TEST_F(ConnectionPoolReturnAndRefreshTest, ConnectionsNotUsedRecentlyArePurged) {
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
 * Verify that a failed connection isn't returned to the pool.
 */
TEST_F(ConnectionPoolFailureTest, ConnectionMarkedFailedIsDroppedOnReturn) {
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
 * Verify that a connection returned with an error indicating the host is unavailable drops
 * all connections to that host.
 */
TEST_F(ConnectionPoolFailureTest, FailedHostErrorsDropConnections) {
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
 * Verify that a connection returned with an error that does not indicate the host is
 * unavailable does not drop other connections to that host.
 */
TEST_F(ConnectionPoolFailureTest, NonFailedHostErrorsDontDropConnections) {
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
 * Verify that requests for different hosts get different connections.
 */
TEST_F(ConnectionPoolCheckoutTest, DifferentHostsDifferentConnections) {
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
 * Verify that a checked-out connection is not reused for a new request for the same host.
 */
TEST_F(ConnectionPoolCheckoutTest, CheckedOutConnectionIsNotReusedForNewRequest) {
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
 * Verify that a reused connection has its status reset to a clean state before being
 * handed to the next caller.
 */
TEST_F(ConnectionPoolCheckoutTest, CheckedOutConnectionStatusIsResetToUnknown) {
    auto pool = makePool();

    // Check out a connection and indicate success so it returns to the ready pool with a
    // non-unknown status.
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          ASSERT(swConn.isOK());
                          doneWith(swConn.getValue());
                      });

    // Check out the same connection again and drop it without indicating success or failure.
    // If the status reset on checkout is working, the pool treats this as a non-network error
    // and drops the connection rather than recycling it.
    ConnectionPool::ConnectionHandle conn;
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          ASSERT(swConn.isOK());
                          conn = std::move(swConn.getValue());
                      });
    dropHandle(conn);

    // The connection is discarded rather than recycled, so the ready pool is empty and a new setup
    // spawns.
    ASSERT_EQ(0, getStats(pool).totalAvailable);
    ASSERT_EQ(static_cast<int>(ConnectionPoolState::kHealthy),
              static_cast<int>(getStats(pool).statsByHost.at(HostAndPort()).poolState));
    ASSERT_EQ(1, ConnectionImpl::setupQueueDepth());
}

/**
 * Verify that an unhealthy connection in the ready pool is dropped at checkout and a fresh
 * connection is established instead.
 */
TEST_F(ConnectionPoolCheckoutTest, UnhealthyReadyConnectionIsDroppedOnCheckout) {
    auto pool = makePool();

    size_t conn1Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          ASSERT(swConn.isOK());
                          conn1Id = getId(swConn.getValue());
                          dynamic_cast<ConnectionImpl*>(swConn.getValue().get())->setUnhealthy();
                          doneWith(swConn.getValue());
                      });

    size_t conn2Id = 0;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          ASSERT(swConn.isOK());
                          conn2Id = getId(swConn.getValue());
                          doneWith(swConn.getValue());
                      });

    ASSERT(conn1Id);
    ASSERT(conn2Id);
    ASSERT_NE(conn1Id, conn2Id);
}

/**
 * Verify that when the caller's acquisition timeout fires first, the error returned is
 * PooledConnectionAcquisitionExceededTimeLimit.
 */
TEST_F(ConnectionPoolQueuingTest,
       AcquisitionTimeoutBeforePendingTimeoutReturnsExceededTimeLimitError) {
    assertTimeoutHelper(
        /* timeout duration */ Milliseconds{100},
        /* expected timeout codes */ ErrorCodes::PooledConnectionAcquisitionExceededTimeLimit);
}

/**
 * Verify that when the pool's pending connection timeout fires first, the error returned is
 * ConnectionEstablishmentTimeout (the connection-establishment timeout now has a
 * dedicated error code rather than the generic HostUnreachable).
 */
TEST_F(ConnectionPoolQueuingTest,
       PendingTimeoutBeforeAcquisitionTimeoutReturnsConnectionEstablishmentTimeoutError) {
    assertTimeoutHelper(
        /* timeout duration */ Milliseconds{500},
        /* expected timeout codes */ ErrorCodes::ConnectionEstablishmentTimeout);
}

/**
 * Verify that an idle connection is refreshed after the refresh requirement timeout elapses.
 */
TEST_F(ConnectionPoolReturnAndRefreshTest,
       IdleConnectionIsRefreshedAfterRefreshRequirementTimeout) {
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

    PoolImpl::setNow(now + Milliseconds(999));
    ASSERT(!refreshedA);

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
 * Verify that refresh can time out.
 */
TEST_F(ConnectionPoolReturnAndRefreshTest, RefreshTimesOut) {
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
 * Verify that requests are served in expiration order, not insertion order.
 */
TEST_F(ConnectionPoolQueuingTest, RequestsAreServedInExpirationOrder) {
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
 * Verify that the pool does not create more connections than the configured maximum.
 */
TEST_F(ConnectionPoolSpawningTest, MaxConnections) {
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
 * Verify that new setups are blocked when the concurrent setup limit is reached.
 */
TEST_F(ConnectionPoolSpawningTest, MaxConnectingLimitCapsNewSetups) {
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
 * Verify that in-progress refreshes count toward the concurrent setup limit and block new setups.
 */
TEST_F(ConnectionPoolSpawningTest, MaxConnectingLimitRefreshBlocksNewSetup) {
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
 * Verify that in-progress refreshes are not themselves subject to the concurrent setup limit.
 */
TEST_F(ConnectionPoolSpawningTest, MaxConnectingLimitDoesNotApplyToRefreshes) {
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
 * Verify that the pool maintains at least the configured minimum number of connections.
 */
TEST_F(ConnectionPoolSpawningTest, MinConnections) {
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
 * Verify that an idle pool's connections are dropped after the host timeout elapses.
 */
TEST_F(ConnectionPoolExpiryTest, IdlePoolExpiresAfterHostTimeout) {
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
 * Verify that the host timeout is delayed as long as there are pending checkout requests.
 */
TEST_F(ConnectionPoolExpiryTest, IdlePoolExpiryIsDelayedByOutstandingRequests) {
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
 * Verify that the host timeout is delayed while connections are checked out.
 */
TEST_F(ConnectionPoolExpiryTest, IdlePoolExpiryIsDelayedByCheckedOutConnections) {
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
 * Verify that pool expiry is delayed while a leased connection is outstanding.
 */
TEST_F(ConnectionPoolExpiryTest, IdlePoolExpiryIsDelayedByLeasedConnections) {
    ConnectionPool::Options options;
    options.refreshRequirement = Milliseconds(5000);
    options.refreshTimeout = Milliseconds(5000);
    options.hostTimeout = Milliseconds(1000);
    auto pool = makePool(options);

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    ConnectionPool::ConnectionHandle leasedConn;
    size_t leasedConnId = 0;

    // Lease a connection.
    ConnectionImpl::pushSetup(Status::OK());
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->lease_forTest(HostAndPort(),
                            Milliseconds(5000),
                            [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                                monitor.exec([&]() {
                                    leasedConnId = verifyAndGetId(swConn);
                                    leasedConn = std::move(swConn.getValue());
                                });
                            });
    });
    ASSERT(leasedConnId);

    // Advance past hostTimeout: pool must NOT expire while the lease is outstanding.
    PoolImpl::setNow(now + Milliseconds(1000));
    ASSERT_EQ(1u, getStats(pool).totalLeased);

    // Release the lease and advance another hostTimeout so the pool expires.
    doneWith(leasedConn);
    PoolImpl::setNow(now + Milliseconds(2000));

    // A new checkout must spawn a fresh connection, confirming the old pool expired.
    size_t newConnId = 0;
    ConnectionImpl::pushSetup(Status::OK());
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  newConnId = verifyAndGetId(swConn);
                                  ASSERT_NE(leasedConnId, newConnId);
                                  doneWith(swConn.getValue());
                              });
                          });
    });
    ASSERT(newConnId);
}

/**
 * Verify that a pool is not destroyed while a connection handle is outstanding.
 */
TEST_F(ConnectionPoolExpiryTest, PoolIsDestroyedAfterAllHandlesAreReleased) {
    ConnectionPool::Options options;
    options.refreshRequirement = Milliseconds(5000);
    options.refreshTimeout = Milliseconds(5000);
    options.hostTimeout = Milliseconds(1000);
    auto pool = makePool(options);

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    ConnectionPool::ConnectionHandle conn;
    size_t connId = 0;

    // Check out a connection.
    ConnectionImpl::pushSetup(Status::OK());
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  connId = verifyAndGetId(swConn);
                                  conn = std::move(swConn.getValue());
                              });
                          });
    });
    ASSERT(connId);

    // The pool must not expire while the connection is checked out.
    PoolImpl::setNow(now + Milliseconds(1000));
    ASSERT_EQ(1u, getStats(pool).totalInUse);

    // Return the connection and advance past another hostTimeout so the pool expires.
    doneWith(conn);
    PoolImpl::setNow(now + Milliseconds(2000));

    // A subsequent checkout spawns a fresh connection, confirming the pool expired.
    size_t newConnId = 0;
    ConnectionImpl::pushSetup(Status::OK());
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(HostAndPort(),
                          Milliseconds(5000),
                          [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                              monitor.exec([&]() {
                                  newConnId = verifyAndGetId(swConn);
                                  ASSERT_NE(connId, newConnId);
                                  doneWith(swConn.getValue());
                              });
                          });
    });
    ASSERT(newConnId);
}

/**
 * Verify that dropping connections for a specific host fails pending requests, causes returned
 * checked-out connections to be discarded, and does not block subsequent checkouts on a stale
 * in-flight refresh.
 */
TEST_F(ConnectionPoolDropTest, DropConnectionsForHost) {
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

/**
 * Verify that a global dropConnections() skips pools marked keepOpen.
 */
TEST_F(ConnectionPoolDropTest, DropConnectionsForAllSkipsKeepOpenPools) {
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
TEST_F(ConnectionPoolSetupTest, SetupTimeoutsFailOtherPendingRequestsWhenPoolIsEmpty) {
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
    ASSERT_EQ(conn1->getStatus(), ErrorCodes::ConnectionEstablishmentTimeout);
    ASSERT(conn2);
    ASSERT(!conn2->isOK());
    // Pending connection fails with the same timeout status.
    ASSERT_EQ(conn2->getStatus(), ErrorCodes::ConnectionEstablishmentTimeout);
}

/**
 * Verify that a setup timeout does not fail other pending requests when established connections
 * exist. Those requests wait until their own acquisition timeout expires instead.
 */
TEST_F(ConnectionPoolSetupTest, SetupTimeoutsDontFailOtherPendingRequestsWhenPoolIsNotEmpty) {
    auto refreshTimeout = Seconds{2};
    auto acquisitionTimeout = Seconds{20};

    boost::optional<StatusWith<ConnectionPool::ConnectionHandle>> connToTriggerSetup;

    auto [pool, inUseConnections] = setupConnectionPool(
        1,
        0,
        1,
        [&](ConnectionPool::Options& options) { options.refreshTimeout = refreshTimeout; },
        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            connToTriggerSetup = std::move(swConn);
        });

    ON_BLOCK_EXIT([&]() {
        for (auto& conn : inUseConnections) {
            doneWith(conn);
        }

        if (connToTriggerSetup && connToTriggerSetup->isOK()) {
            doneWith(connToTriggerSetup->getValue());
        }
    });

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    boost::optional<StatusWith<ConnectionPool::ConnectionHandle>> conn1;
    pool->get_forTest(
        HostAndPort(),
        acquisitionTimeout,
        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) { conn1 = std::move(swConn); });

    // Initially we haven't called our callback.
    ASSERT(!conn1);

    PoolImpl::setNow(now + Seconds(1));

    // Still haven't fired on connToTriggerSetup.
    ASSERT(!conn1);

    // This will trigger a timeout error on the refresh
    PoolImpl::setNow(now + refreshTimeout);

    // But still haven't fired on connToTriggerSetup.
    ASSERT(!conn1);

    PoolImpl::setNow(now + acquisitionTimeout - Milliseconds{1});
    ASSERT(!conn1);

    // The attempt to get a connection times out on acquisitionTimeout
    PoolImpl::setNow(now + acquisitionTimeout);
    ASSERT(conn1);
    ASSERT_EQ(conn1->getStatus(), ErrorCodes::PooledConnectionAcquisitionExceededTimeLimit);
}


/**
 * Verify that a refresh timeout fails all pending requests for the same host.
 */
TEST_F(ConnectionPoolReturnAndRefreshTest, RefreshTimeoutFailsPendingRequests) {
    boost::optional<StatusWith<ConnectionPool::ConnectionHandle>> connToTriggerSetup;

    auto [pool, inUseConnections] = setupConnectionPool(
        1,
        0,
        1,
        [&](ConnectionPool::Options& options) {
            options.refreshTimeout = Seconds(2);
            options.refreshRequirement = Seconds(3);
        },
        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            connToTriggerSetup = std::move(swConn);
        });

    ON_BLOCK_EXIT([&]() {
        for (auto& conn : inUseConnections) {
            doneWith(conn);
        }

        if (connToTriggerSetup && connToTriggerSetup->isOK()) {
            doneWith(connToTriggerSetup->getValue());
        }
    });

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

/**
 * Verify that a refresh timeout drops available connections for the same host.
 */
TEST_F(ConnectionPoolReturnAndRefreshTest, RefreshTimeoutDropsAvailableConnections) {
    boost::optional<StatusWith<ConnectionPool::ConnectionHandle>> connToTriggerSetup;

    const auto refreshTimeout = Seconds{2};
    const auto refreshRequirement = Seconds{3};

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    auto [pool, inUseConnections] = setupConnectionPool(
        0,
        2,
        0,
        [&](ConnectionPool::Options& options) {
            options.refreshTimeout = refreshTimeout;
            options.refreshRequirement = refreshRequirement;
        },
        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            connToTriggerSetup = std::move(swConn);
        });

    ON_BLOCK_EXIT([&]() {
        for (auto& conn : inUseConnections) {
            doneWith(conn);
        }

        if (connToTriggerSetup && connToTriggerSetup->isOK()) {
            doneWith(connToTriggerSetup->getValue());
        }
    });

    // Verify that there are two available connections
    {
        const auto connStats = getStats(pool);
        ASSERT_EQ(0, connStats.totalInUse);
        ASSERT_EQ(2, connStats.totalAvailable);
        ASSERT_EQ(0, connStats.totalRefreshing);
    }

    // Trigger the refresh on both of them
    PoolImpl::setNow(now + refreshRequirement);
    {
        const auto connStats = getStats(pool);
        ASSERT_EQ(0, connStats.totalInUse);
        ASSERT_EQ(0, connStats.totalAvailable);
        ASSERT_EQ(2, connStats.totalRefreshing);
    }

    // Add one of them back to the available pool
    ConnectionImpl::pushRefresh(Status::OK());
    {
        const auto connStats = getStats(pool);
        ASSERT_EQ(0, connStats.totalInUse);
        ASSERT_EQ(1, connStats.totalAvailable);
        ASSERT_EQ(1, connStats.totalRefreshing);
    }

    // Fail the other connection and verify the other one has been dropped.
    PoolImpl::setNow(now + refreshRequirement + refreshTimeout);
    {
        const auto connStats = getStats(pool);
        ASSERT_EQ(0, connStats.totalInUse);
        ASSERT_EQ(0, connStats.totalAvailable);
        ASSERT_EQ(0, connStats.totalRefreshing);
    }
}

/**
 * Verify that dropping a handle without indicating success or failure drops only that single
 * connection, leaves the pool healthy, and spawns a replacement.
 */
TEST_F(ConnectionPoolReturnAndRefreshTest,
       DroppingHandleWithoutIndicatingDropsSingleConnectionOnly) {
    ConnectionPool::Options options;
    options.minConnections = 2;
    options.maxConnections = 2;
    auto pool = makePool(options);

    // Check out both connections.
    ConnectionPool::ConnectionHandle conn1;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          ASSERT(swConn.isOK());
                          conn1 = std::move(swConn.getValue());
                      });

    ConnectionPool::ConnectionHandle conn2;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          ASSERT(swConn.isOK());
                          conn2 = std::move(swConn.getValue());
                      });

    // Drop conn1 without indicating success or failure.
    dropHandle(conn1);

    auto connStats = getStats(pool);
    ASSERT_EQ(0, connStats.totalAvailable);
    ASSERT_EQ(1, connStats.totalInUse);
    auto hostStats = connStats.statsByHost.at(HostAndPort());
    ASSERT_EQ(static_cast<int>(hostStats.poolState),
              static_cast<int>(ConnectionPoolState::kHealthy));
    ASSERT_EQ(1, ConnectionImpl::setupQueueDepth());

    doneWith(conn2);
}


/**
 * Verify that a ConnectionError during refresh drops only that single connection and leaves
 * the pool healthy, unlike other errors that fail the entire pool.
 */
TEST_F(ConnectionPoolReturnAndRefreshTest,
       RefreshFailureWithConnectionErrorDropsSingleConnectionOnly) {
    ConnectionPool::Options options;
    options.minConnections = 0;
    options.refreshRequirement = Milliseconds(1000);
    auto pool = makePool(options);

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    // Check out one connection, keeping it checked out so no ready-pool timer is armed.
    ConnectionPool::ConnectionHandle conn;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          ASSERT(swConn.isOK());
                          conn = std::move(swConn.getValue());
                      });

    // Advance past the refresh requirement while the connection is checked out.
    PoolImpl::setNow(now + Milliseconds(1000));

    // Return the stale connection so it is queued for refresh.
    doneWith(conn);

    // Fail the refresh with a ConnectionError.
    ConnectionImpl::pushRefresh(Status(ErrorCodes::ConnectionError, "connection error"));

    // With minConnections=0, no replacement is spawned after the drop.
    auto connStats = getStats(pool);
    ASSERT_EQ(0, connStats.totalAvailable);
    ASSERT_EQ(0, connStats.totalRefreshing);
    ASSERT_EQ(1, connStats.totalCreated);
    auto hostStats = connStats.statsByHost.at(HostAndPort());
    ASSERT_EQ(static_cast<int>(hostStats.poolState),
              static_cast<int>(ConnectionPoolState::kHealthy));
}

/**
 * Verify that a connection that completes refresh after a failure event is discarded rather than
 * added to the pool.
 */
TEST_F(ConnectionPoolReturnAndRefreshTest, InFlightRefreshCompletedAfterProcessFailureIsDiscarded) {
    ConnectionPool::Options options;
    options.minConnections = 2;
    options.maxConnections = 2;
    options.maxConnecting = 2;
    options.refreshRequirement = Milliseconds(1000);
    auto pool = makePool(options);

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    // Establish both connections (both start checked out).
    ConnectionPool::ConnectionHandle conn1;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          ASSERT(swConn.isOK());
                          conn1 = std::move(swConn.getValue());
                      });

    ConnectionPool::ConnectionHandle conn2;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          ASSERT(swConn.isOK());
                          conn2 = std::move(swConn.getValue());
                      });

    // Advance past the refresh requirement while both connections are checked out.
    PoolImpl::setNow(now + Milliseconds(1000));

    // Return both stale connections so they are queued for refresh.
    doneWith(conn1);
    doneWith(conn2);

    {
        auto connStats = getStats(pool);
        ASSERT_EQ(0, connStats.totalAvailable);
        ASSERT_EQ(2, connStats.totalRefreshing);
    }

    // Fail conn1's refresh with HostUnreachable, putting the pool in a failed state.
    ConnectionImpl::pushRefresh(Status(ErrorCodes::HostUnreachable, "host unreachable"));

    {
        auto stats = getStats(pool);
        auto hostStats = stats.statsByHost.at(HostAndPort());
        ASSERT_EQ(static_cast<int>(hostStats.poolState),
                  static_cast<int>(ConnectionPoolState::kFailed));
    }

    // Fire conn2's refresh successfully. It is discarded because the pool has already failed.
    ConnectionImpl::pushRefresh(Status::OK());

    auto connStats = getStats(pool);
    ASSERT_EQ(0, connStats.totalAvailable);
    ASSERT_EQ(0, connStats.totalRefreshing);
    auto hostStats = connStats.statsByHost.at(HostAndPort());
    ASSERT_EQ(static_cast<int>(hostStats.poolState),
              static_cast<int>(ConnectionPoolState::kFailed));
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

/**
 * Verify that a global dropConnections() removes connections across all host pools, respecting
 * the keepOpen flag to protect individual pools from the drop.
 */
TEST_F(ConnectionPoolDropTest, DropConnectionsViaPool) {
    ConnectionPool::Options options;
    options.minConnections = 0;
    auto pool = makePool(options);

    dropConnectionsTest(pool, pool);
}

/**
 * Verify that dropping connections via EgressConnectionCloserManager produces the same behavior
 * as calling dropConnections() directly on the pool.
 */
TEST_F(ConnectionPoolDropTest, DropConnectionsViaManager) {
    EgressConnectionCloserManager manager;
    ConnectionPool::Options options;
    options.minConnections = 0;
    options.egressConnectionCloserManager = &manager;
    auto pool = makePool(options);

    dropConnectionsTest(pool, &manager);
}

/**
 * Verify that dropping connections fails all pending requests in the queue with
 * PooledConnectionsDropped.
 */
TEST_F(ConnectionPoolDropTest, DropConnectionsFailsPendingRequests) {
    ConnectionPool::Options options;
    options.maxConnections = 1;
    auto pool = makePool(options);

    // Check out the only allowed connection so subsequent requests must queue.
    ConnectionPool::ConnectionHandle handle;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          ASSERT(swConn.isOK());
                          handle = std::move(swConn.getValue());
                      });
    ASSERT(handle);

    // Queue two more requests that cannot be served while the connection is checked out.
    boost::optional<StatusWith<ConnectionPool::ConnectionHandle>> conn1, conn2;
    pool->get_forTest(
        HostAndPort(),
        Milliseconds(5000),
        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) { conn1 = std::move(swConn); });
    pool->get_forTest(
        HostAndPort(),
        Milliseconds(5000),
        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) { conn2 = std::move(swConn); });
    ASSERT(!conn1);
    ASSERT(!conn2);

    // Dropping connections fails both queued requests.
    pool->dropConnections(HostAndPort());

    ASSERT(conn1);
    ASSERT(!conn1->isOK());
    ASSERT_EQ(conn1->getStatus().code(), ErrorCodes::PooledConnectionsDropped);
    ASSERT(conn2);
    ASSERT(!conn2->isOK());
    ASSERT_EQ(conn2->getStatus().code(), ErrorCodes::PooledConnectionsDropped);

    doneWith(handle);
}

/**
 * Verify that a checked-out connection returned after a drop is discarded rather than recycled
 * into the pool.
 */
TEST_F(ConnectionPoolDropTest, CheckedOutConnectionReturnedAfterDropIsDiscarded) {
    ConnectionPool::Options options;
    options.maxConnections = 1;
    auto pool = makePool(options);

    // Check out a connection and hold it.
    ConnectionPool::ConnectionHandle handle;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          ASSERT(swConn.isOK());
                          handle = std::move(swConn.getValue());
                      });
    ASSERT(handle);

    // Drop connections for the host.
    pool->dropConnections(HostAndPort());

    // Return the now-stale handle; it should be discarded, not recycled.
    doneWith(handle);

    // The stale connection must not end up in the ready pool.
    ASSERT_EQ(0u, getStats(pool).totalAvailable);
    ASSERT_EQ(1u, getStats(pool).totalCreated);
}

/**
 * Verify that a request queued while the pool is at capacity is fulfilled once the checked-out
 * connection is returned.
 */
TEST_F(ConnectionPoolQueuingTest, QueuedRequestIsServedWhenConnectionBecomesAvailable) {
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

/**
 * Verify that a pending request whose timeout has already elapsed is rejected cleanly even
 * when the remaining time is negative.
 */
TEST_F(ConnectionPoolQueuingTest, RequestWithNegativeTimeoutIsRejectedImmediately) {
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

TEST_F(ConnectionPoolShutdownTest, ReturnAfterShutdownIsSafe) {
    auto pool = makePool();

    // Grab a connection and hold it to end of scope
    auto connFuture = getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds(1));
    ConnectionImpl::pushSetup(Status::OK());
    auto conn = std::move(connFuture).get();

    pool->shutdown();
    doneWith(conn);
}

TEST_F(ConnectionPoolShutdownTest, GetAfterShutdownReturnsShutdownError) {
    auto pool = makePool();

    // Establish a ready connection so the pool is in a healthy state before shutdown.
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(
        HostAndPort(),
        Milliseconds(5000),
        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) { doneWith(swConn.getValue()); });

    pool->shutdown();

    // get() after shutdown resolves immediately with ShutdownInProgress; no setup is spawned.
    Status gotStatus = Status::OK();
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          gotStatus = swConn.getStatus();
                      });
    EXPECT_EQ(gotStatus.code(), ErrorCodes::ShutdownInProgress);
    EXPECT_EQ(ConnectionImpl::setupQueueDepth(), 0u);
}

TEST_F(ConnectionPoolShutdownTest,
       LeaseAfterShutdownReturnsShutdownErrorWithoutSpawningConnection) {
    auto pool = makePool();

    // Establish a ready connection so the pool is in a healthy state before shutdown.
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(
        HostAndPort(),
        Milliseconds(5000),
        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) { doneWith(swConn.getValue()); });

    pool->shutdown();

    Status gotStatus = Status::OK();
    pool->lease_forTest(HostAndPort(),
                        Milliseconds(5000),
                        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                            gotStatus = swConn.getStatus();
                        });
    EXPECT_EQ(gotStatus.code(), ErrorCodes::ShutdownInProgress);
    EXPECT_EQ(ConnectionImpl::setupQueueDepth(), 0u);
}

TEST_F(ConnectionPoolShutdownTest, ShutdownFailsPendingConnectionRequests) {
    ConnectionPool::Options options;
    options.maxConnections = 1;
    auto pool = makePool(options);

    // Check out the only allowed connection so the pool is at capacity.
    ConnectionPool::ConnectionHandle conn;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          ASSERT_OK(swConn.getStatus());
                          conn = std::move(swConn.getValue());
                      });
    ASSERT(conn);

    // Issue a second get(). The pool is at maxConnections so no new setup is spawned. The request
    // stays queued in _requests waiting for capacity.
    Status gotStatus = Status::OK();
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          gotStatus = swConn.getStatus();
                      });
    ASSERT_EQ(ConnectionImpl::setupQueueDepth(), 0u);

    // shutdown() fails all pending (capacity-blocked) requests with ShutdownInProgress.
    pool->shutdown();

    ASSERT_EQ(gotStatus.code(), ErrorCodes::ShutdownInProgress);
}

TEST_F(ConnectionPoolShutdownTest, ShutdownDiscardsInFlightSetupAndFailsPendingRequests) {
    ConnectionPool::Options options;
    options.maxConnections = 1;
    auto pool = makePool(options);

    // Start a get() — a setup is spawned but not yet completed.
    Status gotStatus = Status::OK();
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          gotStatus = swConn.getStatus();
                      });
    ASSERT_EQ(ConnectionImpl::setupQueueDepth(), 1u);

    pool->shutdown();
    EXPECT_EQ(gotStatus.code(), ErrorCodes::ShutdownInProgress);
    auto stats = getStats(pool);
    EXPECT_EQ(0u, stats.totalAvailable);
    EXPECT_EQ(0u, stats.totalRefreshing);
}

TEST_F(ConnectionPoolShutdownTest, ShutdownDiscardsInFlightRefresh) {
    ConnectionPool::Options options;
    options.maxConnections = 1;
    options.refreshRequirement = Milliseconds(1000);
    options.refreshTimeout = Milliseconds(5000);
    auto pool = makePool(options);

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    // Establish a connection and check it out.
    ConnectionPool::ConnectionHandle conn;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          conn = std::move(swConn.getValue());
                      });
    ASSERT(conn);

    // Advance past refreshRequirement while the connection is checked out so it is stale on
    // return (advancing time here does not fire any refresh timers since the pool is empty).
    PoolImpl::setNow(now + Milliseconds(1500));

    // Return the stale connection — it enters refresh.
    doneWith(conn);
    ASSERT_EQ(1u, getStats(pool).totalRefreshing);

    pool->shutdown();
    auto stats = getStats(pool);
    EXPECT_EQ(0u, stats.totalAvailable);
    EXPECT_EQ(0u, stats.totalRefreshing);
}

TEST_F(ConnectionPoolMetricsTest, SingleCheckoutContributesToTotalUsageTime) {
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

TEST_F(ConnectionPoolMetricsTest, OverlappingCheckoutsAdditivelyContributeToTotalUsageTime) {
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

/**
 * Verify that a concurrent lease does not count toward the pool's total connection usage time.
 */
TEST_F(ConnectionPoolLeasingTest, LeasedConnectionsDontCountTowardsConnectionUsageTime) {
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
                            ASSERT_OK(swConn.getStatus());
                            PoolImpl::setNow(endTimePoint);
                            doneWith(swConn.getValue());
                        });

    ConnectionPoolStats finalStats;
    pool->appendConnectionStats(&finalStats);

    auto totalTimeUsageDelta = finalStats.totalConnUsageTime - initialStats.totalConnUsageTime;
    ASSERT_EQ(totalTimeUsageDelta, Milliseconds(0));
}

/**
 * Verify that a concurrent lease does not inflate the connection usage time when there is a
 * concurrent checkout.
 */
TEST_F(ConnectionPoolLeasingTest,
       LeasedConnectionsWithConcurrentCheckoutDontInflateConnectionUsageTime) {
    constexpr Milliseconds checkOutLength = Milliseconds(10);
    auto pool = makePool();

    ConnectionPoolStats initialStats;
    pool->appendConnectionStats(&initialStats);

    auto startTimePoint = Date_t::now();
    auto endTimePoint = startTimePoint + checkOutLength;
    PoolImpl::setNow(startTimePoint);

    ConnectionImpl::pushSetup(Status::OK());
    ConnectionImpl::pushSetup(Status::OK());

    // Check out one connection and lease one connection.
    ConnectionPool::ConnectionHandle normal;
    ConnectionPool::ConnectionHandle leased;
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          ASSERT_OK(swConn.getStatus());
                          normal = std::move(swConn.getValue());
                      });
    pool->lease_forTest(HostAndPort(),
                        Milliseconds(5000),
                        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                            ASSERT_OK(swConn.getStatus());
                            leased = std::move(swConn.getValue());
                        });

    // Verify both handles were acquired before testing time behavior.
    ASSERT(normal);
    ASSERT(leased);

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

/**
 * Verify that a returned leased connection is available for reuse via get() without spawning a
 * new setup.
 */
TEST_F(ConnectionPoolLeasingTest, ReturnedLeasedConnectionIsAvailableForReuse) {
    auto pool = makePool();

    // Lease one connection.
    ConnectionPool::ConnectionHandle leased;
    size_t leasedId = 0;
    ConnectionImpl::pushSetup(Status::OK());
    pool->lease_forTest(HostAndPort(),
                        Milliseconds(5000),
                        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                            leasedId = verifyAndGetId(swConn);
                            leased = std::move(swConn.getValue());
                        });
    ASSERT(leased);

    // Return the connection. It should go back to the ready pool.
    doneWith(leased);
    ASSERT_EQ(1u, getStats(pool).totalAvailable);

    // Get the connection. No new setup should be needed.
    size_t reusedId = 0;
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          reusedId = verifyAndGetId(swConn);
                          doneWith(swConn.getValue());
                      });
    ASSERT_EQ(0u, ConnectionImpl::setupQueueDepth());
    ASSERT_EQ(leasedId, reusedId);
    ASSERT_EQ(1u, getStats(pool).totalCreated);
}

/**
 * Verify that a pending lease() request is fulfilled when a connection becomes available.
 */
TEST_F(ConnectionPoolLeasingTest, PendingLeaseIsFulfilledWhenConnectionBecomesAvailable) {
    ConnectionPool::Options options;
    options.maxConnections = 1;
    auto pool = makePool(options);

    // Check out the only connection so the pool is at capacity.
    ConnectionPool::ConnectionHandle normal;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          ASSERT_OK(swConn.getStatus());
                          normal = std::move(swConn.getValue());
                      });
    ASSERT(normal);

    // Enqueue a lease(). The pool is at maxConnections so no new setup is spawned.
    ConnectionPool::ConnectionHandle leased;
    pool->lease_forTest(HostAndPort(),
                        Milliseconds(5000),
                        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                            ASSERT_OK(swConn.getStatus());
                            leased = std::move(swConn.getValue());
                        });
    ASSERT_FALSE(leased);

    // Return the checked-out connection. It should be routed to the pending lease request.
    doneWith(normal);
    ASSERT_TRUE(leased);
    ASSERT_EQ(0u, ConnectionImpl::setupQueueDepth());
    ASSERT_EQ(0u, getStats(pool).totalAvailable);
    ASSERT_EQ(1u, getStats(pool).totalLeased);

    doneWith(leased);
}

/**
 * Verify that a leased connection counts toward the connection limit.
 */
TEST_F(ConnectionPoolLeasingTest, LeasedConnectionCountsTowardsMaxConnections) {
    ConnectionPool::Options options;
    options.maxConnections = 1;
    auto pool = makePool(options);

    // Lease one connection. It counts against the connection limit.
    ConnectionPool::ConnectionHandle leased;
    ConnectionImpl::pushSetup(Status::OK());
    pool->lease_forTest(HostAndPort(),
                        Milliseconds(5000),
                        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                            ASSERT_OK(swConn.getStatus());
                            leased = std::move(swConn.getValue());
                        });
    ASSERT_TRUE(leased);

    // A get() request cannot be served while the pool is at its connection limit.
    bool gotConn = false;
    ConnectionPool::ConnectionHandle conn;
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          ASSERT_OK(swConn.getStatus());
                          gotConn = true;
                          conn = std::move(swConn.getValue());
                      });
    ASSERT_FALSE(gotConn);

    // Returning the leased connection frees capacity and unblocks the pending get().
    doneWith(leased);
    ASSERT_TRUE(gotConn);
    doneWith(conn);
}

TEST_F(ConnectionPoolCancellationTest, CancelGetPreCancelledRejectsRequest) {
    CancellationSource source;
    auto pool = makePool();

    source.cancel();
    auto connFuture =
        getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds(1), source.token());
    ASSERT_TRUE(connFuture.isReady());
    ASSERT_THROWS_CODE(connFuture.get(), DBException, ErrorCodes::CallbackCanceled);
}

TEST_F(ConnectionPoolCancellationTest, CancelGetWhilePendingRejectsRequest) {
    CancellationSource source;
    auto pool = makePool();

    auto connFuture =
        getFromPool(HostAndPort(), transport::kGlobalSSLMode, Seconds(1), source.token());
    ASSERT_FALSE(connFuture.isReady());

    source.cancel();
    ASSERT_TRUE(connFuture.isReady());
    ASSERT_THROWS_CODE(connFuture.get(), DBException, ErrorCodes::CallbackCanceled);
}

TEST_F(ConnectionPoolCancellationTest, CancelGetEarlySkipsReadyConnection) {
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

TEST_F(ConnectionPoolCancellationTest, CancelGetLateGetsConnection) {
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

/**
 * Verify that cancelling a token after the pool has been destroyed is safe and does not
 * crash or access freed memory.
 */
TEST_F(ConnectionPoolCancellationTest, CancelGetAfterDestruction) {
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

TEST_F(ConnectionPoolCancellationTest,
       CancellationSourceDestroyedWithoutCancelDoesNotAffectRequest) {
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

TEST_F(ConnectionPoolMetricsTest, ConnectionFailureReasonIsLogged) {
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

TEST_F(ConnectionPoolMetricsTest, ConnectionAcquisitionWaitTimeIsTrackedPerHost) {
    auto pool = makePool();

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    // Issue a get() without completing setup so the request waits in the queue.
    bool gotConn = false;
    ConnectionPool::ConnectionHandle conn;
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          if (!swConn.isOK())
                              return;
                          gotConn = true;
                          conn = std::move(swConn.getValue());
                      });

    // Advance the virtual clock before completing setup so that wait time is non-zero.
    PoolImpl::setNow(now + Milliseconds(10));

    // Setup completes and the elapsed wait time is recorded in pool stats.
    ConnectionImpl::pushSetup(Status::OK());
    ASSERT_TRUE(gotConn);

    doneWith(conn);

    auto stats = getStats(pool);
    ASSERT_GTE(stats.totalConnectionAcquisitionWaitTime, Milliseconds(10));
    auto hostStats = stats.statsByHost.at(HostAndPort());
    ASSERT_GTE(hostStats.connectionAcquisitionWaitTime, Milliseconds(10));
}

TEST_F(ConnectionPoolMetricsTest, ConnectionStatsCoverAllHosts) {
    const HostAndPort host1("host1:27017");
    const HostAndPort host2("host2:27017");
    auto pool = makePool();

    // Establish and return one connection to each host.
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(
        host1, Milliseconds(5000), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            doneWith(swConn.getValue());
        });
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(
        host2, Milliseconds(5000), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            doneWith(swConn.getValue());
        });

    auto stats = getStats(pool);
    EXPECT_EQ(2u, stats.totalCreated);
    EXPECT_EQ(2u, stats.totalAvailable);
    EXPECT_EQ(2u, stats.statsByHost.size());
    EXPECT_EQ(1u, stats.statsByHost.at(host1).created);
    EXPECT_EQ(1u, stats.statsByHost.at(host1).available);
    EXPECT_EQ(1u, stats.statsByHost.at(host2).created);
    EXPECT_EQ(1u, stats.statsByHost.at(host2).available);
}

// SERVER-68329: a ConnectionError setup failure (e.g. asio::error::in_progress
// race in the transport layer) drops only the failing connection without flushing the pool.
TEST_F(ConnectionPoolTest, SetupFailureWithConnectionErrorDoesNotDropOpenConnections) {
    auto [pool, inUseConnections] = setupConnectionPool(
        1,
        1,
        1,
        [&](ConnectionPool::Options& options) { options.refreshTimeout = Seconds(100); },
        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {});

    ON_BLOCK_EXIT([&]() {
        for (auto& conn : inUseConnections) {
            doneWith(conn);
        }
    });

    ConnectionImpl::pushSetup(Status(ErrorCodes::ConnectionError, "rate-limited"));

    // The failing pending connection is dropped, but the in-use and available connections survive
    // and the pool spawns a replacement pending connection.
    {
        const auto connStats = getStats(pool);
        ASSERT_EQ(1, connStats.totalInUse);
        ASSERT_EQ(1, connStats.totalAvailable);
        ASSERT_EQ(1, connStats.totalRefreshing);
    }

    // Return the in-use connection; it should not be refreshed away.
    {
        auto conn = std::move(inUseConnections.back());
        inUseConnections.pop_back();
        doneWithAsync(conn).get();

        const auto connStats = getStats(pool);
        ASSERT_EQ(0, connStats.totalInUse);
        ASSERT_EQ(2, connStats.totalAvailable);
        ASSERT_EQ(1, connStats.totalRefreshing);
    }

    // Contrarily, when a setup connection fails with a non-ConnectionError, the available
    // connections get dropped via processFailure.
    ConnectionImpl::pushSetup(Status(ErrorCodes::NetworkInterfaceExceededTimeLimit, ""));
    {
        const auto connStats = getStats(pool);
        ASSERT_EQ(0, connStats.totalInUse);
        ASSERT_EQ(0, connStats.totalRefreshing);
        ASSERT_EQ(0, connStats.totalAvailable);
    }
}

// Any setup failure that is not ConnectionError and is not one of the transient
// rate-limiter signals (ConnectionClosedByPeer / ConnectionEstablishmentTimeout) must drop open
// connections via processFailure, even when the pool already has established connections. This
// covers plain HostUnreachable (e.g. a failed or refused connect, or an otherwise-unhealthy host)
// and unrelated network errors. Note on timeouts: a timeout during
// connect or the initial hello is reported as ConnectionEstablishmentTimeout and single-drops (so
// it is not in this flush list); a timeout during the later authentication step is reported as
// HostUnreachable, which flushes -- so HostUnreachable is exercised here.
TEST_F(ConnectionPoolTest, SetupFailureWithNonConnectionErrorDropsOpenConnections) {
    const std::vector<ErrorCodes::Error> nonConnectionErrorFailures = {
        ErrorCodes::HostUnreachable,
        ErrorCodes::SocketException,
        ErrorCodes::NetworkTimeout,
        ErrorCodes::AuthenticationFailed,
        ErrorCodes::HostNotFound,
        ErrorCodes::SSLHandshakeFailed,
    };

    for (auto ec : nonConnectionErrorFailures) {
        auto [pool, inUseConnections] = setupConnectionPool(
            1,
            3,
            1,
            [&](ConnectionPool::Options& options) { options.refreshTimeout = Seconds(100); },
            [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {});

        ON_BLOCK_EXIT([&]() {
            for (auto& conn : inUseConnections) {
                doneWith(conn);
            }
        });

        // Sanity check: pool starts with one in-use, three available, one refreshing.
        {
            const auto connStats = getStats(pool);
            ASSERT_EQ(1, connStats.totalInUse) << "ec=" << ErrorCodes::errorString(ec);
            ASSERT_EQ(3, connStats.totalAvailable) << "ec=" << ErrorCodes::errorString(ec);
            ASSERT_EQ(1, connStats.totalRefreshing) << "ec=" << ErrorCodes::errorString(ec);
        }

        // Inject the non-ConnectionError setup failure. The pool must process it as a real failure:
        // the available pool and any in-flight refreshes are cleared and the pool transitions to
        // kFailed. Currently checked-out (in-use) connections survive processFailure -- they are
        // owned by the caller and only become stale via the generation bump.
        ConnectionImpl::pushSetup(Status(ec, "non-ConnectionError setup failure"));

        const auto connStats = getStats(pool);
        ASSERT_EQ(1, connStats.totalInUse) << "ec=" << ErrorCodes::errorString(ec);
        ASSERT_EQ(0, connStats.totalAvailable) << "ec=" << ErrorCodes::errorString(ec);
        ASSERT_EQ(0, connStats.totalRefreshing) << "ec=" << ErrorCodes::errorString(ec);

        const auto hostStats = connStats.statsByHost.at(HostAndPort());
        ASSERT_EQ(static_cast<int>(hostStats.poolState),
                  static_cast<int>(ConnectionPoolState::kFailed))
            << "ec=" << ErrorCodes::errorString(ec);
    }
}

// A transient rate-limiter signal during setup -- ConnectionClosedByPeer (the peer closed the
// socket: asio::error::eof for non-TLS, asio::ssl::error::stream_truncated for TLS) or
// ConnectionEstablishmentTimeout (setup timed out) -- must single-drop the failing attempt, not
// flush the pool, when the pool already has established connections. finishRefresh keys on the code
// (not the reason string), so we inject the code directly.
TEST_F(ConnectionPoolTest, RateLimiterRejectionsDoNotClearPool) {
    for (auto ec :
         {ErrorCodes::ConnectionClosedByPeer, ErrorCodes::ConnectionEstablishmentTimeout}) {
        auto [pool, inUseConnections] = setupConnectionPool(
            1,
            3,
            1,
            [&](ConnectionPool::Options& options) { options.refreshTimeout = Seconds(100); },
            [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {});

        ON_BLOCK_EXIT([&]() {
            for (auto& conn : inUseConnections) {
                doneWith(conn);
            }
        });

        // Sanity: pool starts with one in-use, three available, one refreshing.
        {
            const auto s = getStats(pool);
            ASSERT_EQ(1, s.totalInUse) << "ec=" << ErrorCodes::errorString(ec);
            ASSERT_EQ(3, s.totalAvailable) << "ec=" << ErrorCodes::errorString(ec);
            ASSERT_EQ(1, s.totalRefreshing) << "ec=" << ErrorCodes::errorString(ec);
        }

        // The failing pending connection is single-dropped; the in-use and the three available
        // connections must survive and a replacement is spawned.
        ConnectionImpl::pushSetup(Status(ec, "transient single-drop signal"));
        {
            const auto s = getStats(pool);
            ASSERT_EQ(1, s.totalInUse) << "ec=" << ErrorCodes::errorString(ec);
            ASSERT_EQ(3, s.totalAvailable) << "ec=" << ErrorCodes::errorString(ec);
            ASSERT_EQ(1, s.totalRefreshing) << "ec=" << ErrorCodes::errorString(ec);
        }

        // Returning the in-use connection keeps the pool healthy.
        {
            auto conn = std::move(inUseConnections.back());
            inUseConnections.pop_back();
            doneWithAsync(conn).get();

            const auto s = getStats(pool);
            ASSERT_EQ(0, s.totalInUse) << "ec=" << ErrorCodes::errorString(ec);
            ASSERT_EQ(4, s.totalAvailable) << "ec=" << ErrorCodes::errorString(ec);
            ASSERT_EQ(1, s.totalRefreshing) << "ec=" << ErrorCodes::errorString(ec);
        }

        // Drain the replacement pending connection before the next iteration / teardown.
        ConnectionImpl::pushSetup(Status::OK());
    }
}

// Negative case: the same signals with no established connections have nothing to protect, so they
// fall through to processFailure and clear the pool.
TEST_F(ConnectionPoolTest, RateLimiterRejectionsClearPoolWhenNoEstablishedConnections) {
    for (auto ec :
         {ErrorCodes::ConnectionClosedByPeer, ErrorCodes::ConnectionEstablishmentTimeout}) {
        auto [pool, inUseConnections] = setupConnectionPool(
            0,
            0,
            1,
            [&](ConnectionPool::Options& options) { options.refreshTimeout = Seconds(100); },
            [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {});

        // No established connections -- pool is entirely in the "refreshing" (setup) state.
        {
            const auto s = getStats(pool);
            ASSERT_EQ(0, s.totalInUse) << "ec=" << ErrorCodes::errorString(ec);
            ASSERT_EQ(0, s.totalAvailable) << "ec=" << ErrorCodes::errorString(ec);
            ASSERT_EQ(1, s.totalRefreshing) << "ec=" << ErrorCodes::errorString(ec);
        }

        // The failure triggers processFailure and clears the pool.
        ConnectionImpl::pushSetup(Status(ec, "transient single-drop signal"));
        {
            const auto s = getStats(pool);
            ASSERT_EQ(0, s.totalInUse) << "ec=" << ErrorCodes::errorString(ec);
            ASSERT_EQ(0, s.totalAvailable) << "ec=" << ErrorCodes::errorString(ec);
            ASSERT_EQ(0, s.totalRefreshing) << "ec=" << ErrorCodes::errorString(ec);
        }
    }
}

/**
 * Verify that after a refresh failure, the pool spawns a replacement connection once
 * kHostRetryTimeout has elapsed.
 */
TEST_F(ConnectionPoolFailureTest, RefreshFailureSpawnsNewConnectionsAfterRetryTimeout) {

    boost::optional<StatusWith<ConnectionPool::ConnectionHandle>> connToTriggerSetup;

    const auto refreshTimeout = Milliseconds(2);
    const auto refreshRequirement = Milliseconds(3);

    const auto startTimePoint = Date_t::now();
    PoolImpl::setNow(startTimePoint);

    auto [pool, inUseConnections] = setupConnectionPool(
        1,
        1,
        0,
        [&](ConnectionPool::Options& options) {
            options.refreshTimeout = refreshTimeout;
            options.refreshRequirement = refreshRequirement;
        },
        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            connToTriggerSetup = std::move(swConn);
        });

    ON_BLOCK_EXIT([&]() {
        for (auto& conn : inUseConnections) {
            doneWith(conn);
        }

        if (connToTriggerSetup && connToTriggerSetup->isOK()) {
            doneWith(connToTriggerSetup->getValue());
        }
    });

    // Init test status:
    {
        const auto connStats = getStats(pool);
        ASSERT_EQ(1, connStats.totalInUse);
        ASSERT_EQ(1, connStats.totalAvailable);
        ASSERT_EQ(0, connStats.totalRefreshing);
    }

    // Force a refresh on the available connection
    PoolImpl::setNow(startTimePoint + refreshRequirement);
    {
        const auto connStats = getStats(pool);
        ASSERT_EQ(1, connStats.totalInUse);
        ASSERT_EQ(0, connStats.totalAvailable);
        ASSERT_EQ(1, connStats.totalRefreshing);
    }

    // Make it fail and check that we stopped having available and refreshing connections
    ConnectionImpl::pushRefresh(Status(ErrorCodes::HostUnreachable, ""));
    {
        const auto connStats = getStats(pool);
        ASSERT_EQ(1, connStats.totalInUse);
        ASSERT_EQ(0, connStats.totalAvailable);
        ASSERT_EQ(0, connStats.totalRefreshing);
    }
    PoolImpl::setNow(startTimePoint + ConnectionPool::kHostRetryTimeout - Milliseconds{1});
    {
        const auto connStats = getStats(pool);
        ASSERT_EQ(1, connStats.totalInUse);
        ASSERT_EQ(0, connStats.totalAvailable);
        ASSERT_EQ(0, connStats.totalRefreshing);
    }

    // New connection will be spawned after kHostRetryTimeout
    PoolImpl::setNow(startTimePoint + ConnectionPool::kHostRetryTimeout);
    {
        const auto connStats = getStats(pool);
        ASSERT_EQ(1, connStats.totalInUse);
        ASSERT_EQ(0, connStats.totalAvailable);
        ASSERT_EQ(1, connStats.totalRefreshing);
    }
}

/**
 * Verify that after a refresh failure, the pool spawns a replacement connection when the next
 * checkout request arrives, even before kHostRetryTimeout has elapsed.
 */
TEST_F(ConnectionPoolFailureTest, RefreshFailureSpawnsNewConnectionOnNextCheckout) {

    boost::optional<StatusWith<ConnectionPool::ConnectionHandle>> connToTriggerSetup;

    const auto refreshTimeout = Milliseconds(2);
    const auto refreshRequirement = Milliseconds(3);

    const auto startTimePoint = Date_t::now();
    PoolImpl::setNow(startTimePoint);

    auto [pool, inUseConnections] = setupConnectionPool(
        0,
        1,
        0,
        [&](ConnectionPool::Options& options) {
            options.refreshTimeout = refreshTimeout;
            options.refreshRequirement = refreshRequirement;
        },
        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            connToTriggerSetup = std::move(swConn);
        });

    ON_BLOCK_EXIT([&]() {
        for (auto& conn : inUseConnections) {
            doneWith(conn);
        }

        if (connToTriggerSetup && connToTriggerSetup->isOK()) {
            doneWith(connToTriggerSetup->getValue());
        }
    });

    // Initial test status:
    {
        const auto connStats = getStats(pool);
        ASSERT_EQ(0, connStats.totalInUse);
        ASSERT_EQ(1, connStats.totalAvailable);
        ASSERT_EQ(0, connStats.totalRefreshing);
    }

    // Force a refresh on the available connection
    auto lastTimePoint = startTimePoint + refreshRequirement;
    PoolImpl::setNow(lastTimePoint);
    {
        const auto connStats = getStats(pool);
        ASSERT_EQ(0, connStats.totalInUse);
        ASSERT_EQ(0, connStats.totalAvailable);
        ASSERT_EQ(1, connStats.totalRefreshing);
    }

    // Make it fail and check that we stopped having available and refreshing connections
    ConnectionImpl::pushRefresh(Status(ErrorCodes::HostUnreachable, ""));
    {
        const auto connStats = getStats(pool);
        ASSERT_EQ(0, connStats.totalInUse);
        ASSERT_EQ(0, connStats.totalAvailable);
        ASSERT_EQ(0, connStats.totalRefreshing);
    }

    // Sill not new refreshing connections
    PoolImpl::setNow(lastTimePoint + ConnectionPool::kHostRetryTimeout - Milliseconds{50});
    {
        const auto connStats = getStats(pool);
        ASSERT_EQ(0, connStats.totalInUse);
        ASSERT_EQ(0, connStats.totalAvailable);
        ASSERT_EQ(0, connStats.totalRefreshing);
    }

    // New connection will be spawned once a connection request kicks in
    boost::optional<StatusWith<ConnectionPool::ConnectionHandle>> conn1;
    pool->get_forTest(
        HostAndPort(), Seconds(10), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            conn1 = std::move(swConn);
        });
    ASSERT(!conn1);

    {
        const auto connStats = getStats(pool);
        ASSERT_EQ(0, connStats.totalInUse);
        ASSERT_EQ(0, connStats.totalAvailable);
        ASSERT_EQ(1, connStats.totalRefreshing);
    }

    ConnectionImpl::pushSetup(Status::OK());
    ASSERT(conn1);
    doneWith(conn1->getValue());
}

/**
 * Verify that a pool with no failures reports a healthy state.
 */
TEST_F(ConnectionPoolFailureTest, HealthyPoolReportsHealthyState) {
    auto pool = makePool();

    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(
        HostAndPort(),
        Milliseconds(5000),
        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) { doneWith(swConn.getValue()); });

    auto stats = getStats(pool);
    auto hostStats = stats.statsByHost.at(HostAndPort());
    ASSERT_EQ(static_cast<int>(hostStats.poolState),
              static_cast<int>(ConnectionPoolState::kHealthy));
}

/**
 * Verify that a pool that received a network error from a returned connection reports a failed
 * state.
 */
TEST_F(ConnectionPoolFailureTest, FailedPoolReportsFailedState) {
    auto pool = makePool();

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(),
                      Milliseconds(5000),
                      [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                          doneWithError(swConn.getValue(), {ErrorCodes::HostUnreachable, "error"});
                      });

    auto stats = getStats(pool);
    auto hostStats = stats.statsByHost.at(HostAndPort());
    ASSERT_EQ(static_cast<int>(hostStats.poolState),
              static_cast<int>(ConnectionPoolState::kFailed));
}

/**
 * Verify that a setup failure with no established connections causes pool failure.
 */
TEST_F(ConnectionPoolSetupTest, SetupFailureWithNoEstablishedConnectionsCausesPoolFailure) {
    auto pool = makePool();

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    boost::optional<StatusWith<ConnectionPool::ConnectionHandle>> conn1;
    pool->get_forTest(
        HostAndPort(), Seconds(10), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            conn1 = std::move(swConn);
        });

    ConnectionImpl::pushSetup(Status(ErrorCodes::HostUnreachable, "host unreachable"));

    ASSERT(conn1);
    ASSERT(!conn1->isOK());
    ASSERT_EQ(conn1->getStatus(), ErrorCodes::HostUnreachable);

    auto stats = getStats(pool);
    auto hostStats = stats.statsByHost.at(HostAndPort());
    ASSERT_EQ(static_cast<int>(hostStats.poolState),
              static_cast<int>(ConnectionPoolState::kFailed));
}

/**
 * Verify that a setup failure with multiple in-flight setups and no established connections causes
 * pool failure.
 */
TEST_F(ConnectionPoolSetupTest,
       SetupFailureWithMultipleInFlightSetupsAndNoEstablishedCausesPoolFailure) {
    ConnectionPool::Options options;
    options.minConnections = 3;
    options.maxConnections = 3;
    options.maxConnecting = 3;
    auto pool = makePool(options);

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    boost::optional<StatusWith<ConnectionPool::ConnectionHandle>> conn1;
    pool->get_forTest(
        HostAndPort(), Seconds(10), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            conn1 = std::move(swConn);
        });

    {
        auto stats = getStats(pool);
        auto hostStats = stats.statsByHost.at(HostAndPort());
        ASSERT_EQ(0, hostStats.available);
        ASSERT_EQ(0, hostStats.inUse);
        ASSERT_EQ(3, hostStats.refreshing);
    }

    ConnectionImpl::pushSetup(Status(ErrorCodes::HostUnreachable, "host unreachable"));

    ASSERT(conn1);
    ASSERT(!conn1->isOK());
    ASSERT_EQ(conn1->getStatus(), ErrorCodes::HostUnreachable);

    auto stats = getStats(pool);
    auto hostStats = stats.statsByHost.at(HostAndPort());
    ASSERT_EQ(static_cast<int>(hostStats.poolState),
              static_cast<int>(ConnectionPoolState::kFailed));
}

/**
 * Verify that consecutive setup failures do not block new connection attempts. The pool
 * continues to spawn replacements after each failure.
 */
TEST_F(ConnectionPoolSetupTest, MultipleConsecutiveSetupFailuresDoNotBlockNewConnections) {
    boost::optional<StatusWith<ConnectionPool::ConnectionHandle>> connToTriggerSetup;

    auto [pool, inUseConnections] = setupConnectionPool(
        1,
        0,
        3,
        [&](ConnectionPool::Options& options) { options.refreshTimeout = Seconds(100); },
        [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            connToTriggerSetup = std::move(swConn);
        });

    ON_BLOCK_EXIT([&]() {
        for (auto& conn : inUseConnections) {
            doneWith(conn);
        }

        if (connToTriggerSetup && connToTriggerSetup->isOK()) {
            doneWith(connToTriggerSetup->getValue());
        }
    });

    {
        const auto connStats = getStats(pool);
        ASSERT_EQ(1, connStats.totalInUse);
        ASSERT_EQ(0, connStats.totalAvailable);
        ASSERT_EQ(3, connStats.totalRefreshing);
    }

    // Push a single-drop failure of each code in sequence (establishedConnections > 0, so all three
    // take the single-drop path). The pool immediately spawns a replacement for each, so
    // consecutive failures never block it.
    for (auto ec : {ErrorCodes::ConnectionError,
                    ErrorCodes::ConnectionClosedByPeer,
                    ErrorCodes::ConnectionEstablishmentTimeout}) {
        ConnectionImpl::pushSetup(Status(ec, ""));
        const auto connStats = getStats(pool);
        ASSERT_EQ(1, connStats.totalInUse) << "ec=" << ErrorCodes::errorString(ec);
        ASSERT_EQ(3, connStats.totalRefreshing) << "ec=" << ErrorCodes::errorString(ec);
    }

    // Verify the pool is still healthy (not in failed state) because there's an established
    // connection.
    auto stats = getStats(pool);
    auto hostStats = stats.statsByHost.at(HostAndPort());
    ASSERT_EQ(static_cast<int>(hostStats.poolState),
              static_cast<int>(ConnectionPoolState::kHealthy));
}


/**
 * Verify that a connection that completes setup after a failure event is discarded rather than
 * added to the pool.
 */
TEST_F(ConnectionPoolSetupTest, InFlightSetupCompletedAfterProcessFailureIsDiscarded) {
    ConnectionPool::Options options;
    options.minConnections = 3;
    options.maxConnecting = 3;
    auto pool = makePool(options);

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    boost::optional<StatusWith<ConnectionPool::ConnectionHandle>> conn1;
    pool->get_forTest(
        HostAndPort(), Seconds(10), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            conn1 = std::move(swConn);
        });

    {
        auto connStats = getStats(pool);
        ASSERT_EQ(0, connStats.totalAvailable);
        ASSERT_EQ(0, connStats.totalInUse);
        ASSERT_EQ(3, connStats.totalRefreshing);
    }

    // One setup failure puts the pool in a failed state with two remaining in-flight setups.
    ConnectionImpl::pushSetup(Status(ErrorCodes::HostUnreachable, "host unreachable"));

    ASSERT(conn1);
    ASSERT(!conn1->isOK());

    {
        auto stats = getStats(pool);
        auto hostStats = stats.statsByHost.at(HostAndPort());
        ASSERT_EQ(static_cast<int>(hostStats.poolState),
                  static_cast<int>(ConnectionPoolState::kFailed));
    }

    // The two remaining in-flight setups complete successfully but are discarded because the
    // pool has already failed.
    ConnectionImpl::pushSetup(Status::OK());
    ConnectionImpl::pushSetup(Status::OK());

    auto connStats = getStats(pool);
    ASSERT_EQ(0, connStats.totalAvailable);
    ASSERT_EQ(0, connStats.totalRefreshing);
    auto hostStats = connStats.statsByHost.at(HostAndPort());
    ASSERT_EQ(static_cast<int>(hostStats.poolState),
              static_cast<int>(ConnectionPoolState::kFailed));
}

/**
 * A controller that behaves like LimitController but always returns canShutdown = false.
 * This simulates the ShardingTaskExecutorPoolController behavior where other members of a replica
 * set are still healthy, preventing an expired pool from being shut down.
 */
class NeverShutdownLimitController final : public ConnectionPool::ControllerInterface {
public:
    void addHost(PoolId id, const HostAndPort& host) override {
        std::lock_guard lk(_mutex);
        _poolData[id] = {host, 0};
    }

    HostGroupState updateHost(PoolId id, const PoolMetrics& stats) override {
        std::lock_guard lk(_mutex);
        auto& data = _poolData[id];
        auto opts = getPoolOptions();

        data.target = stats.requests + stats.active + stats.leased;
        if (data.target < opts.minConnections) {
            data.target = opts.minConnections;
        } else if (data.target > opts.maxConnections) {
            data.target = opts.maxConnections;
        }

        return {{data.host}, false};
    }

    void removeHost(PoolId id) override {
        std::lock_guard lk(_mutex);
        _poolData.erase(id);
    }

    ConnectionControls getControls(PoolId id) override {
        std::lock_guard lk(_mutex);
        return {getPoolOptions().maxConnecting, _poolData[id].target};
    }

    Milliseconds hostTimeout() const override {
        return getPoolOptions().hostTimeout;
    }
    Milliseconds pendingTimeout() const override {
        return getPoolOptions().refreshTimeout;
    }
    Milliseconds toRefreshTimeout() const override {
        return getPoolOptions().refreshRequirement;
    }
    size_t connectionRequestsMaxQueueDepth() const override {
        return getPoolOptions().connectionRequestsMaxQueueDepth;
    }
    size_t maxConnections() const override {
        return getPoolOptions().maxConnections;
    }
    std::string_view name() const override {
        return "NeverShutdownLimitController"sv;
    }
    void updateConnectionPoolStats(ConnectionPoolStats*) const override {}

private:
    struct PoolData {
        HostAndPort host;
        size_t target = 0;
    };
    std::mutex _mutex;
    stdx::unordered_map<PoolId, PoolData> _poolData;
};

/**
 * Verify that a pool that expires while in a failed state does not immediately retry connections
 * without waiting for kHostRetryTimeout. Regression test for a bug where expiring an
 * already-failed pool caused the kHostRetryTimeout backoff to be bypassed, creating a tight
 * retry loop.
 */
TEST_F(ConnectionPoolFailureTest, FailedPoolExpiredDoesNotRetryWithoutBackoff) {
    ConnectionPool::Options options;
    options.hostTimeout = Milliseconds(1);
    options.refreshRequirement = Milliseconds(5);
    options.refreshTimeout = Seconds(20);
    options.minConnections = 1;
    options.maxConnections = 1;
    options.maxConnecting = 1;
    options.controllerFactory = []() -> std::shared_ptr<ConnectionPool::ControllerInterface> {
        return std::make_shared<NeverShutdownLimitController>();
    };
    auto pool = makePool(std::move(options));

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    // Establish a connection successfully.
    boost::optional<StatusWith<ConnectionPool::ConnectionHandle>> conn;
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(HostAndPort(), Seconds(10), [&](auto swConn) { conn = std::move(swConn); });
    ASSERT(conn);
    ASSERT(conn->isOK());

    // Return the connection, making the pool idle. _lastActiveTime is set to now.
    doneWith(conn->getValue());
    ASSERT_EQ(1u, getStats(pool).totalCreated);

    // Advance time past both hostTimeout (1ms) and refreshRequirement (5ms). This fires the
    // connection's refresh timer (putting it in the processing pool for refresh) and ensures
    // _hostExpiration <= now so the pool is eligible for the kExpired transition.
    PoolImpl::setNow(now + Milliseconds(6));

    // Fail the refresh. This calls processFailure() which should set the pool to kFailed.
    // The kFailed state must prevent spawnConnections() from immediately retrying.
    ConnectionImpl::pushRefresh(Status(ErrorCodes::ShutdownInProgress, "in quiesce mode"));

    // Correct behavior: no new connection is spawned immediately because the pool should remain
    // in kFailed, and spawnConnections() bails out for kFailed pools.
    ASSERT_EQ(0u, ConnectionImpl::setupQueueDepth());

    auto stats = getStats(pool);
    auto hostStats = stats.statsByHost.at(HostAndPort());
    ASSERT_EQ(static_cast<int>(hostStats.poolState),
              static_cast<int>(ConnectionPoolState::kFailed));

    // After kHostRetryTimeout, the event timer fires and transitions the pool back to kHealthy,
    // allowing spawnConnections() to spawn a new connection.
    PoolImpl::setNow(now + Milliseconds(6) + ConnectionPool::kHostRetryTimeout);
    ASSERT_EQ(1u, ConnectionImpl::setupQueueDepth());
}

/**
 * Verify that the kHostRetryTimeout backoff is re-enforced on each subsequent failure, not just
 * the first. After a second failure the pool must again wait a full kHostRetryTimeout before
 * spawning new connections.
 */
TEST_F(ConnectionPoolFailureTest, FailedPoolEnforcesBackoffOnEachSubsequentFailureCycle) {
    auto pool = makePool();

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    // Request a connection to trigger a setup.
    boost::optional<StatusWith<ConnectionPool::ConnectionHandle>> conn;
    pool->get_forTest(
        HostAndPort(), Seconds(60), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            conn = std::move(swConn);
        });

    // First setup fails, putting the pool in a failed state.
    ConnectionImpl::pushSetup(Status(ErrorCodes::HostUnreachable, "host unreachable"));
    ASSERT(conn);
    ASSERT(!conn->isOK());

    // No new setup before the first kHostRetryTimeout.
    PoolImpl::setNow(now + ConnectionPool::kHostRetryTimeout - Milliseconds{1});
    ASSERT_EQ(0u, ConnectionImpl::setupQueueDepth());

    // At kHostRetryTimeout the pool recovers and spawns a new setup.
    PoolImpl::setNow(now + ConnectionPool::kHostRetryTimeout);
    ASSERT_EQ(1u, ConnectionImpl::setupQueueDepth());

    // Second setup fails, putting the pool in a failed state again.
    ConnectionImpl::pushSetup(Status(ErrorCodes::HostUnreachable, "host unreachable"));

    // No new setup before the second kHostRetryTimeout (measured from the second failure).
    PoolImpl::setNow(now + ConnectionPool::kHostRetryTimeout * 2 - Milliseconds{1});
    ASSERT_EQ(0u, ConnectionImpl::setupQueueDepth());

    // At the second kHostRetryTimeout another new setup is spawned.
    PoolImpl::setNow(now + ConnectionPool::kHostRetryTimeout * 2);
    ASSERT_EQ(1u, ConnectionImpl::setupQueueDepth());
}

// Reproduces a use-after-free where the cancellation callback registered in
// SpecificPool::getConnection captures a raw `this` pointer without a shared_from_this() anchor.
// The sequence is:
//   1. getConnection() registers an onCancel callback capturing raw `this`
//   2. The caller's CancellationToken is cancelled, which queues the callback on the executor
//   3. pool->shutdown() runs, which destroys the SpecificPool (all shared_from_this anchors are
//      released when connections and timers are cleaned up)
//   4. The queued callback fires, dereferencing the dangling `this` to access _parent->_mutex
//
// The InlineQueuedCountingExecutor serializes tasks: when a task is already running, newly
// scheduled tasks are queued. By cancelling the token and shutting down the pool inside one
// executor task, we guarantee the onCancel callback is deferred until after the SpecificPool
// is destroyed.
TEST_F(ConnectionPoolCancellationTest, CancellationCallbackSurvivesPoolDestruction) {
    CancellationSource source;
    auto pool = makePool();

    // Request a leased connection with a cancellation token. Don't push setup so the request
    // stays pending. Using lease() avoids the .tap() callback in the non-lease path that would
    // otherwise prevent the SpecificPool from being destroyed (it captures a shared_ptr to it).
    auto connFuture =
        ExecutorFuture(_executor)
            .then([pool, token = source.token()]() {
                return pool->lease(HostAndPort(), transport::kGlobalSSLMode, Seconds(1), token);
            })
            .semi();
    ASSERT_FALSE(connFuture.isReady());

    // Run cancel + shutdown inside a single executor task. Because InlineQueuedCountingExecutor
    // defers tasks scheduled while another task is running:
    //   - source.cancel() triggers the onCancel future chain, which calls
    //     executor->schedule() for the callback → it is QUEUED, not run yet.
    //   - pool->shutdown() then runs synchronously: processFailure() fulfils all pending
    //     requests with an error, connections and timers are torn down (releasing all
    //     shared_from_this anchors), and the SpecificPool is destroyed when shutdown()'s
    //     local `pools` vector goes out of scope.
    //   - pool.reset() / dropPool() release the ConnectionPool itself.
    //
    // After this task finishes the executor drains its queue. The onCancel callback runs with
    // Status::OK (the child token was cancelled, not dismissed) and dereferences the now-dangling
    // `this` pointer → use-after-free.
    //
    // The fix is to capture `anchor = shared_from_this()` in the onCancel callback, matching
    // every other async callback in SpecificPool (guardCallback, makeHandle deleter, etc.).
    _executor->schedule([&](Status) {
        source.cancel();
        pool->shutdown();
        pool.reset();
        dropPool();
    });

    // connFuture was resolved (by processFailure during shutdown or by the cancel callback).
    ASSERT_TRUE(connFuture.isReady());
    ASSERT_NOT_OK(std::move(connFuture).getNoThrow());
}

// Controller that groups two hosts into the same HostGroupState and returns
// canShutdown = stats.isExpired, mirroring the default LimitController's policy but
// spanning two hosts.
class HostGroupLimitController final : public ConnectionPool::ControllerInterface {
public:
    HostGroupLimitController(HostAndPort primary, HostAndPort secondary)
        : _primary(std::move(primary)), _secondary(std::move(secondary)) {}

    void addHost(PoolId id, const HostAndPort& host) override {
        std::lock_guard lk(_mutex);
        _poolData[id] = {host, 0};
    }

    HostGroupState updateHost(PoolId id, const PoolMetrics& stats) override {
        std::lock_guard lk(_mutex);
        auto& data = _poolData[id];
        auto opts = getPoolOptions();

        data.target = stats.requests + stats.active + stats.leased;
        if (data.target < opts.minConnections) {
            data.target = opts.minConnections;
        } else if (data.target > opts.maxConnections) {
            data.target = opts.maxConnections;
        }

        return {{_primary, _secondary}, stats.isExpired};
    }

    void removeHost(PoolId id) override {
        std::lock_guard lk(_mutex);
        if (auto it = _poolData.find(id); it != _poolData.end()) {
            _removedHosts.insert(it->second.host);
            _poolData.erase(it);
        }
    }

    bool wasRemoved(const HostAndPort& host) const {
        std::lock_guard lk(_mutex);
        return _removedHosts.count(host) > 0;
    }

    ConnectionControls getControls(PoolId id) override {
        std::lock_guard lk(_mutex);
        return {getPoolOptions().maxConnecting, _poolData[id].target};
    }

    Milliseconds hostTimeout() const override {
        return getPoolOptions().hostTimeout;
    }
    Milliseconds pendingTimeout() const override {
        return getPoolOptions().refreshTimeout;
    }
    Milliseconds toRefreshTimeout() const override {
        return getPoolOptions().refreshRequirement;
    }
    size_t connectionRequestsMaxQueueDepth() const override {
        return getPoolOptions().connectionRequestsMaxQueueDepth;
    }
    size_t maxConnections() const override {
        return getPoolOptions().maxConnections;
    }
    std::string_view name() const override {
        return "HostGroupLimitController"sv;
    }
    void updateConnectionPoolStats(ConnectionPoolStats*) const override {}

private:
    struct PoolData {
        HostAndPort host;
        size_t target = 0;
    };
    HostAndPort _primary;
    HostAndPort _secondary;
    mutable std::mutex _mutex;
    stdx::unordered_map<PoolId, PoolData> _poolData;
    std::set<HostAndPort> _removedHosts;
};

/**
 * Verify that a network failure on one host in a host group does not affect the other host's pool.
 */
TEST_F(ConnectionPoolFailureTest, HostGroupFailureOnOneHostDoesNotAffectOtherHosts) {
    const HostAndPort primary("primary:27017");
    const HostAndPort secondary("secondary:27017");

    auto controller = std::make_shared<HostGroupLimitController>(primary, secondary);
    ConnectionPool::Options options;
    options.controllerFactory =
        [controller]() -> std::shared_ptr<ConnectionPool::ControllerInterface> {
        return controller;
    };
    auto pool = makePool(options);

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    // Establish and return a connection to each host.
    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(
        primary, Milliseconds(5000), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            doneWith(swConn.getValue());
        });

    ConnectionImpl::pushSetup(Status::OK());
    pool->get_forTest(
        secondary, Milliseconds(5000), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            doneWith(swConn.getValue());
        });

    // Return a primary connection with a network error, triggering pool failure.
    pool->get_forTest(
        primary, Milliseconds(5000), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            doneWithError(swConn.getValue(), {ErrorCodes::HostUnreachable, "error"});
        });

    auto stats = getStats(pool);
    ASSERT_EQ(static_cast<int>(stats.statsByHost.at(primary).poolState),
              static_cast<int>(ConnectionPoolState::kFailed));

    // The secondary pool should remain healthy with its connection available.
    ASSERT_EQ(static_cast<int>(stats.statsByHost.at(secondary).poolState),
              static_cast<int>(ConnectionPoolState::kHealthy));
    ASSERT_EQ(1u, stats.statsByHost.at(secondary).available);

    // A request to the secondary is served from the ready pool without a new setup.
    pool->get_forTest(
        secondary, Milliseconds(5000), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
            ASSERT_OK(swConn.getStatus());
            doneWith(swConn.getValue());
        });
    ASSERT_EQ(0u, ConnectionImpl::setupQueueDepth());
}

// Idle pools have no incoming requests or returning connections to trigger a state update.
// Instead, a repeating timer fires periodically, and that timer is the only mechanism by which
// an idle pool can detect that it has expired and shut itself down.
//
// Verify that the timer respects the configured host timeout.
TEST_F(ConnectionPoolExpiryTest, HostGroupPoolExpiresAfterHostTimeout) {
    const HostAndPort primary("primary:27017");
    const HostAndPort secondary("secondary:27017");

    // hostTimeout is set to 2s (two timer periods) so the idle timer fires exactly twice:
    // once at 1s before hostTimeout has elapsed, and once at 2s when hostTimeout has elapsed
    // and the pool should expire.
    auto controller = std::make_shared<HostGroupLimitController>(primary, secondary);
    ConnectionPool::Options options;
    options.hostTimeout = Milliseconds(2000);
    options.controllerFactory =
        [controller]() -> std::shared_ptr<ConnectionPool::ControllerInterface> {
        return controller;
    };
    auto pool = makePool(options);

    auto now = Date_t::now();
    PoolImpl::setNow(now);

    // A get() to the primary causes the controller to create the secondary pool as well.
    // The secondary's idle timer is set to fire at now+kHostRetryTimeout.
    ConnectionImpl::pushSetup(Status::OK());
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        pool->get_forTest(
            primary, Milliseconds(60000), [&](StatusWith<ConnectionPool::ConnectionHandle> swConn) {
                monitor.exec([&]() {
                    ASSERT_OK(swConn.getStatus());
                    doneWith(swConn.getValue());
                });
            });
    });

    // Advance past kHostRetryTimeout so the idle timer fires for the first time.
    // hostTimeout has not yet elapsed so the pool should survive.
    PoolImpl::setNow(now + Milliseconds(1001));
    ASSERT(!controller->wasRemoved(primary))
        << "Primary pool must not be destroyed before hostTimeout elapses";
    ASSERT(!controller->wasRemoved(secondary))
        << "Secondary pool must not be destroyed before hostTimeout elapses";

    // Advance past hostTimeout (2*kHostRetryTimeout) so the idle timer fires a second time.
    // hostTimeout has elapsed so the pool should be removed.
    PoolImpl::setNow(now + Milliseconds(2001));
    ASSERT(controller->wasRemoved(primary)) << "Primary pool should have expired after hostTimeout";
    ASSERT(controller->wasRemoved(secondary))
        << "Secondary pool should have expired after hostTimeout";
}

TYPED_TEST(ConnectionPoolLimitControllerTest, LimitControllerTargetConnectionsCalculation) {
    ConnectionPool::Options opts;
    opts.minConnections = 5;
    opts.maxConnections = 20;
    auto [pool, controller] = this->setupLimitController(opts);
    ConnectionPool::PoolMetrics metrics;

    controller->addHost(0, HostAndPort());

    // Verify connection target is set to minConnections when the host is idle.
    controller->updateHost(0, metrics);
    auto controls = controller->getControls(0);
    ASSERT_EQ(controls.targetConnections, opts.minConnections);

    // Verify connection target is set to the sum of requests + active + leased when that sum
    // is within the bounds of [minConnections, maxConnections].
    metrics.leased = 1;
    metrics.active = 2;
    metrics.pending = 3;
    metrics.ready = 4;
    metrics.requests = 5;

    controller->updateHost(0, metrics);
    controls = controller->getControls(0);
    ASSERT_EQ(controls.targetConnections, metrics.requests + metrics.active + metrics.leased);

    // Verify connection target does not exceed maxConnections, even when demand exceeds this value.
    metrics.requests = 100;

    controller->updateHost(0, metrics);
    controls = controller->getControls(0);
    ASSERT_EQ(controls.targetConnections, opts.maxConnections);
}

TYPED_TEST(ConnectionPoolLimitControllerTest, LimitControllerCanShutdownFollowsExpiry) {
    auto [pool, controller] = this->setupLimitController();
    ConnectionPool::PoolMetrics metrics;

    controller->addHost(0, HostAndPort());

    auto state = controller->updateHost(0, metrics);
    ASSERT_EQ(state.canShutdown, false);

    metrics.isExpired = true;
    state = controller->updateHost(0, metrics);
    ASSERT_EQ(state.canShutdown, true);
}

// Test that verifies the host target connection calculation is updated when the controller's
// dynamic loader functions return different values. This ensures that the controller is properly
// calling the dynamic loader functions on each update and using their return values in the
// target connection calculation.
TEST_F(DynamicLimitControllerTest, DynamicLoaderValuesAreReflectedInTargetConnections) {
    size_t minValue = 1;
    size_t maxValue = 10;

    auto [pool, controller] =
        makeDynamicController([&] { return minValue; }, [&] { return maxValue; });

    ConnectionPool::PoolMetrics metrics;
    metrics.requests = 0;
    metrics.active = 0;
    metrics.leased = 0;

    addHostAndUpdate(controller, 0, HostAndPort("localhost:27017"), metrics);
    ASSERT_EQ(controller->getControls(0).targetConnections, minValue);

    minValue = 5;
    maxValue = 20;
    updateHostAndGetControls(controller, 0, metrics);
    ASSERT_EQ(controller->getControls(0).targetConnections, minValue);
}

// Verify that adding and removing a host from the controller does not cause any crashes.
// This is a basic sanity test to ensure that the controller can handle dynamic changes
// to the host list without issues.
TEST_F(DynamicLimitControllerTest, AddAndRemoveHostTrackingDoesNotCrash) {
    auto [pool, controller] = makeDynamicController(1, 2);
    controller->addHost(0, HostAndPort("localhost:27017"));
    controller->removeHost(0);

    SUCCEED();
}

// Verify that the controller properly clamps the target connections to the min and max values
// returned by the dynamic loader functions. This ensures that even if the dynamic loader
// functions return values that would normally be outside the allowed range, the controller
// still enforces the configured limits.
TEST_F(DynamicLimitControllerTest, UpdateHostClampsDemandToMinAndMax) {
    size_t minValue = 5;
    size_t maxValue = 10;
    auto [pool, controller] =
        makeDynamicController([&] { return minValue; }, [&] { return maxValue; });

    ConnectionPool::PoolMetrics metrics;
    metrics.requests = 0;
    metrics.active = 0;
    metrics.leased = 0;
    addHostAndUpdate(controller, 0, HostAndPort("localhost:27017"), metrics);
    ASSERT_EQ(controller->getControls(0).targetConnections, minValue);

    metrics.requests = 100;
    updateHostAndGetControls(controller, 0, metrics);
    ASSERT_EQ(controller->getControls(0).targetConnections, maxValue);
}

// Verify that the controller re-evaluates the dynamic loader functions on each updateHost() call,
// and that changes to the values returned by those functions are reflected in the target connection
// calculation. This ensures that the controller is not caching the results of the dynamic loader
// functions and is properly using their return values on each update.
TEST_F(DynamicLimitControllerTest, UpdateHostRechecksDynamicBoundsOnEachCall) {
    size_t minValue = 5;
    size_t maxValue = 10;
    auto [pool, controller] =
        makeDynamicController([&] { return minValue; }, [&] { return maxValue; });

    ConnectionPool::PoolMetrics metrics;
    metrics.requests = 100;
    metrics.active = 0;
    metrics.leased = 0;

    addHostAndUpdate(controller, 0, HostAndPort("localhost:27017"), metrics);
    ASSERT_EQ(controller->getControls(0).targetConnections, maxValue);

    maxValue = 50;
    metrics.requests = 20;
    updateHostAndGetControls(controller, 0, metrics);
    ASSERT_EQ(controller->getControls(0).targetConnections,
              metrics.requests + metrics.active + metrics.leased);
}

// Verify that removing and re-adding a host causes the controller to reset any cached state for
// that host, allowing it to properly re-calculate the target connections based on the current
// metrics and dynamic loader values. This ensures that the controller can handle hosts being
// removed and re-added without retaining stale state that would affect its calculations.
TEST_F(DynamicLimitControllerTest, RemoveAndReAddHostAllowsFreshTargetCalculation) {
    size_t minValue = 1;
    size_t maxValue = 10;
    auto [pool, controller] =
        makeDynamicController([&] { return minValue; }, [&] { return maxValue; });

    ConnectionPool::PoolMetrics metrics;
    metrics.requests = 2;
    metrics.active = 1;
    metrics.leased = 0;

    addHostAndUpdate(controller, 0, HostAndPort("localhost:27017"), metrics);
    ASSERT_EQ(controller->getControls(0).targetConnections,
              metrics.requests + metrics.active + metrics.leased);

    controller->removeHost(0);
    controller->addHost(0, HostAndPort("localhost:27017"));
    updateHostAndGetControls(controller, 0, metrics);
    ASSERT_EQ(controller->getControls(0).targetConnections,
              metrics.requests + metrics.active + metrics.leased);
}

// Sanity test that verifies the controller's name() method returns the expected string.
TEST_F(DynamicLimitControllerTest, NameReturnsConfiguredControllerName) {
    auto [pool, controller] = makeDynamicController(1, 2);
    ASSERT_EQ(controller->name(), "dynamic limit controller");
}

// Verify that the controller's getControls() method returns the maxConnecting value from the pool
// options, rather than a hardcoded value. This ensures that the controller is properly using the
// pool's configuration when determining the controls to return.
TEST_F(DynamicLimitControllerTest, UsesPoolMaxConnectingInGetControls) {
    ConnectionPool::Options opts;
    opts.maxConnecting = 17;
    auto [pool, controller] = makeDynamicController(1, 2, opts);
    controller->addHost(0, HostAndPort("localhost:27017"));

    ASSERT_EQ(controller->getControls(0).maxPendingConnections, opts.maxConnecting);
}

}  // namespace connection_pool_test_details
}  // namespace executor
}  // namespace mongo
