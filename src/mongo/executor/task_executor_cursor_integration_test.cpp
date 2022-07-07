/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/executor/task_executor_cursor.h"

#include "mongo/client/dbclient_base.h"
#include "mongo/db/concurrency/locker_noop_client_observer.h"
#include "mongo/db/namespace_string.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace executor {
namespace {

class TaskExecutorCursorFixture : public mongo::unittest::Test {
public:
    TaskExecutorCursorFixture() {
        _serviceCtx->registerClientObserver(std::make_unique<LockerNoopClientObserver>());
    }

    void setUp() override {
        std::shared_ptr<NetworkInterface> ni = makeNetworkInterface("TaskExecutorCursorTest");
        auto tp = std::make_unique<NetworkInterfaceThreadPool>(ni.get());

        _executor = std::make_unique<ThreadPoolTaskExecutor>(std::move(tp), std::move(ni));
        _executor->startup();
    };

    void tearDown() override {
        _executor->shutdown();
        _executor.reset();
    };

    TaskExecutor* executor() {
        return _executor.get();
    }

    auto makeOpCtx() {
        return _client->makeOperationContext();
    }

    TaskExecutor::CallbackHandle scheduleRemoteCommand(OperationContext* opCtx,
                                                       HostAndPort target,
                                                       BSONObj cmd) {
        LOGV2(6531702, "About to run a remote command", "cmd"_attr = cmd);
        RemoteCommandRequest rcr(target, "admin", cmd, opCtx);
        auto swHandle = executor()->scheduleRemoteCommand(
            std::move(rcr), [](const TaskExecutor::RemoteCommandCallbackArgs&) {});
        return uassertStatusOK(swHandle);
    }

    void runRemoteCommand(OperationContext* opCtx, HostAndPort target, BSONObj cmd) {
        auto cbHandle = scheduleRemoteCommand(opCtx, std::move(target), cmd);
        executor()->wait(cbHandle, opCtx);
        LOGV2(6531703, "Finished running remote command", "cmd"_attr = cmd);
    }

private:
    ServiceContext::UniqueServiceContext _serviceCtx = ServiceContext::make();
    std::unique_ptr<ThreadPoolTaskExecutor> _executor;
    ServiceContext::UniqueClient _client = _serviceCtx->makeClient("TaskExecutorCursorTest");
};

size_t createTestData(std::string ns, size_t numDocs) {
    auto swConn = unittest::getFixtureConnectionString().connect("TaskExecutorCursorTest");
    uassertStatusOK(swConn.getStatus());
    auto dbclient = std::move(swConn.getValue());

    std::vector<BSONObj> docs;
    docs.reserve(numDocs);
    for (size_t i = 0; i < numDocs; ++i) {
        docs.emplace_back(BSON("x" << int(i)));
    }
    dbclient->dropCollection(ns);
    dbclient->insert(ns, docs);
    return dbclient->count(NamespaceString(ns));
}

// Test that we can actually use a TaskExecutorCursor to read multiple batches from a remote host
TEST_F(TaskExecutorCursorFixture, Basic) {
    const size_t numDocs = 100;
    ASSERT_EQ(createTestData("test.test", numDocs), numDocs);

    auto opCtx = makeOpCtx();
    RemoteCommandRequest rcr(unittest::getFixtureConnectionString().getServers().front(),
                             "test",
                             BSON("find"
                                  << "test"
                                  << "batchSize" << 10),
                             opCtx.get());

    TaskExecutorCursor tec(executor(), rcr, [] {
        TaskExecutorCursor::Options opts;
        opts.batchSize = 10;
        return opts;
    }());

    size_t count = 0;
    while (auto doc = tec.getNext(opCtx.get())) {
        count++;
    }

    ASSERT_EQUALS(count, numDocs);
}

/**
 * Verifies that the underlying connection used to run `getMore` commands remains open, even after
 * the instance of `TaskExecutorCursor` is destroyed.
 *
 * The test goes through the following steps:
 * - Load test data into "test.test".
 * - Make sure there are enough connections to run the test.
 * - Configure a fail-point to block the initial `getMore` that populates the cursor.
 * - Create an instance of `TaskExecutorCursor`. The executor will send out an asynchronous
 *   `getMore` to populate the cursor, but the command does not return so long as the fail-point
 *   is enabled.
 * - Count the total number of connections available before destroying `TaskExecutorCursor`.
 * - Destroy the instance of `TaskExecutorCursor`, disable the fail-point, and wait for all
 *   connections to become idle.
 * - Recount the number of connections and verify that no connection is closed.
 *
 * See SERVER-65317 for more context.
 */
TEST_F(TaskExecutorCursorFixture, ConnectionRemainsOpenAfterKillingTheCursor) {
    const size_t numDocs = 100;
    ASSERT_EQ(createTestData("test.test", numDocs), numDocs);

    auto opCtx = makeOpCtx();
    const auto target = unittest::getFixtureConnectionString().getServers().front();

    auto getConnectionStatsForTarget = [&] {
        ConnectionPoolStats stats;
        executor()->appendConnectionStats(&stats);
        return stats.statsByHost[target];
    };

    // We only need at most four connections to run this test, which will run the following
    // commands: `find`, `getMore`, `killCursor`, and `configureFailPoint`. Thus, the rest of this
    // test won't need to create new connections and the number of open connections should remain
    // unchanged.
    const size_t kNumConnections = 4;
    std::vector<TaskExecutor::CallbackHandle> handles;
    auto cmd = BSON("find"
                    << "test"
                    << "filter"
                    << BSON("$where"
                            << "sleep(100); return true;"));
    for (size_t i = 0; i < kNumConnections; i++) {
        handles.emplace_back(scheduleRemoteCommand(opCtx.get(), target, cmd));
    }
    for (auto cbHandle : handles) {
        executor()->wait(cbHandle);
    }

    ConnectionStatsPer beforeStats;
    {
        const auto fpName = "waitAfterCommandFinishesExecution";
        runRemoteCommand(opCtx.get(),
                         target,
                         BSON("configureFailPoint"
                              << fpName << "mode"
                              << "alwaysOn"
                              << "data"
                              << BSON("ns"
                                      << "test.test"
                                      << "commands" << BSON_ARRAY("getMore"))));
        ScopeGuard guard([&] {
            runRemoteCommand(opCtx.get(),
                             target,
                             BSON("configureFailPoint" << fpName << "mode"
                                                       << "off"));
        });

        RemoteCommandRequest rcr(target,
                                 "test",
                                 BSON("find"
                                      << "test"
                                      << "batchSize" << 10),
                                 opCtx.get());

        TaskExecutorCursor tec(executor(), rcr, [] {
            TaskExecutorCursor::Options opts;
            opts.batchSize = 10;
            return opts;
        }());

        tec.populateCursor(opCtx.get());

        // At least one of the connections is busy running the initial `getMore` command to populate
        // the cursor. The command is blocked on the remote host and does not return until after the
        // destructor for `tec` returns.
        beforeStats = getConnectionStatsForTarget();
        ASSERT_GTE(beforeStats.inUse, 1);
    }

    // Wait for all connections to become idle -- this ensures all tasks scheduled as part of
    // cleaning up `tec` have finished running.
    while (getConnectionStatsForTarget().inUse > 0) {
        LOGV2(6531701, "Waiting for all connections to become idle");
        sleepFor(Seconds(1));
    }

    const auto afterStats = getConnectionStatsForTarget();
    auto countOpenConns = [](const ConnectionStatsPer& stats) {
        return stats.inUse + stats.available + stats.refreshing;
    };

    // Verify that no connection is created or closed.
    ASSERT_EQ(beforeStats.created, afterStats.created);
    ASSERT_EQ(countOpenConns(beforeStats), countOpenConns(afterStats));
}

}  // namespace
}  // namespace executor
}  // namespace mongo
