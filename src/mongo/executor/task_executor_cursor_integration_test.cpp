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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/executor_integration_test_connection_stats.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/network_interface_tl.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/grpc_connection_stats_gen.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace executor {
namespace {
constexpr auto kNetworkInterfaceInstanceName = "TaskExecutorCursorTest"_sd;

std::pair<int, int> getCreatedAndOpenConnectionStats(std::shared_ptr<TaskExecutor> executor,
                                                     StringData subObjName,
                                                     const HostAndPort& remote,
                                                     bool collectGRPCStats) {
    int created = 0;
    int open = 0;

    if (collectGRPCStats) {
        BSONObjBuilder bob;
        executor->appendNetworkInterfaceStats(bob);
        auto stats = GRPCConnectionStats::parse(bob.obj().getObjectField(subObjName),
                                                IDLParserContext("GRPCStats"));
        open = stats.getTotalOpenChannels();
    } else {
        ConnectionPoolStats stats;
        executor->appendConnectionStats(&stats);

        ConnectionStatsPer hostStats = stats.statsByHost[remote];
        created = hostStats.created;
        open = hostStats.inUse + hostStats.available + hostStats.refreshing + hostStats.leased;
    }
    return {created, open};
}

class TaskExecutorCursorFixture : public mongo::unittest::Test {
public:
    TaskExecutorCursorFixture() = default;

    void setUp() override {
        if (!unittest::shouldUseGRPCEgress()) {
            _ni = makeNetworkInterface(
                kNetworkInterfaceInstanceName, nullptr, nullptr, ConnectionPool::Options());
        } else {
#ifdef MONGO_CONFIG_GRPC
            _ni = makeNetworkInterfaceGRPC(kNetworkInterfaceInstanceName);
#else
            MONGO_UNREACHABLE;
#endif
        }
        auto tp = std::make_unique<NetworkInterfaceThreadPool>(_ni.get());

        _executor = ThreadPoolTaskExecutor::create(std::move(tp), _ni);
        _executor->startup();
    };

    void tearDown() override {
        _executor->shutdown();
        _executor->join();
        _executor.reset();
    };

    std::shared_ptr<TaskExecutor> executor() {
        return _executor;
    }

    auto net() {
        return _ni.get();
    }

    auto makeOpCtx() {
        return _client->makeOperationContext();
    }

    TaskExecutor::CallbackHandle scheduleRemoteCommand(OperationContext* opCtx,
                                                       HostAndPort target,
                                                       BSONObj cmd) {
        LOGV2(6531702, "About to run a remote command", "cmd"_attr = cmd);
        RemoteCommandRequest rcr(target, DatabaseName::kAdmin, cmd, opCtx);
        auto swHandle = executor()->scheduleRemoteCommand(
            std::move(rcr), [](const TaskExecutor::RemoteCommandCallbackArgs&) {});
        return uassertStatusOK(swHandle);
    }

    void runRemoteCommand(OperationContext* opCtx, HostAndPort target, BSONObj cmd) {
        auto cbHandle = scheduleRemoteCommand(opCtx, std::move(target), cmd);
        executor()->wait(cbHandle, opCtx);
        LOGV2(6531703, "Finished running remote command", "cmd"_attr = cmd);
    }

    const AsyncClientFactory& getFactory() {
        return checked_cast<NetworkInterfaceTL*>(net())->getClientFactory_forTest();
    }

private:
    std::shared_ptr<ThreadPoolTaskExecutor> _executor;
    std::shared_ptr<NetworkInterface> _ni;
    ServiceContext::UniqueClient _client =
        getGlobalServiceContext()->getService()->makeClient("TaskExecutorCursorTest");
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
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(ns);

    dbclient->dropCollection(nss);
    dbclient->insert(nss, docs);
    return dbclient->count(nss);
}

// Test that we can actually use a TaskExecutorCursor to read multiple batches from a remote host.
TEST_F(TaskExecutorCursorFixture, Basic) {
    const size_t numDocs = 100;
    ASSERT_EQ(createTestData("test.test", numDocs), numDocs);

    auto opCtx = makeOpCtx();
    RemoteCommandRequest rcr(unittest::getFixtureConnectionString().getServers().front(),
                             DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                             BSON("find" << "test"
                                         << "batchSize" << 10),
                             opCtx.get());

    executor::TaskExecutorCursorOptions opts(/*pinConnection*/ gPinTaskExecCursorConns.load() ||
                                                 unittest::shouldUseGRPCEgress(),
                                             /*batchSize*/ 10);
    auto tec = std::make_unique<TaskExecutorCursor>(executor(), rcr, std::move(opts));

    size_t count = 0;
    while (auto doc = tec->getNext(opCtx.get())) {
        count++;
    }

    ASSERT_EQUALS(count, numDocs);
}

// Test that a TaskExecutorCursor can be used in a non-pinning mode to read multiple
// batches from a remote host. The default mode for TaskExecutorCursor is pinned.
TEST_F(TaskExecutorCursorFixture, BasicNonPinned) {
    const size_t numDocs = 100;
    ASSERT_EQ(createTestData("test.test", numDocs), numDocs);

    auto opCtx = makeOpCtx();
    RemoteCommandRequest rcr(unittest::getFixtureConnectionString().getServers().front(),
                             DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                             BSON("find" << "test"
                                         << "batchSize" << 10),
                             opCtx.get());
    executor::TaskExecutorCursorOptions opts(/*pinConnection*/ false,
                                             /*batchSize*/ 10,
                                             /*preFetchNextBatch*/ true,
                                             /*planYieldPolicy*/ nullptr);
    auto tec = std::make_unique<TaskExecutorCursor>(executor(), rcr, std::move(opts));

    size_t count = 0;
    while (auto doc = tec->getNext(opCtx.get())) {
        count++;
    }

    ASSERT_EQUALS(count, numDocs);
}

// Test that when a TaskExecutorCursor is used in pinning-mode, the pinned executor's destruction
// is scheduled on the underlying executor.
TEST_F(TaskExecutorCursorFixture, PinnedExecutorDestroyedOnUnderlying) {
    const size_t numDocs = 100;
    ASSERT_EQ(createTestData("test.test", numDocs), numDocs);

    auto opCtx = makeOpCtx();
    RemoteCommandRequest rcr(unittest::getFixtureConnectionString().getServers().front(),
                             DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                             BSON("find" << "test"
                                         << "batchSize" << 10),
                             opCtx.get());

    executor::TaskExecutorCursorOptions opts(/*pinConnection*/ true,
                                             /*batchSize*/ 10,
                                             /*preFetchNextBatch*/ true,
                                             /*planYieldPolicy*/ nullptr);
    auto tec = std::make_unique<TaskExecutorCursor>(executor(), rcr, std::move(opts));

    // Fetch a documents to make sure the TEC was initialized properly.
    ASSERT(tec->getNext(opCtx.get()));
    // Enable the failpoint in the integration test process.
    {
        FailPointEnableBlock fpb("blockBeforePinnedExecutorIsDestroyedOnUnderlying");
        auto initialTimesEntered = fpb.initialTimesEntered();
        // Destroy the TEC and ensure we reach the code block that will destroy the pinned executor.
        tec.reset();
        LOGV2(7361301, "Waiting for TaskExecutorCursor to destroy its pinning executor.");
        fpb->waitForTimesEntered(initialTimesEntered + 1);
    }
    // Allow the pinned executor's destruction to proceed.
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

    bool usingGRPC = unittest::shouldUseGRPCEgress();

    // We only need at most four connections to run this test, which will run the following
    // commands: `find`, `getMore`, `killCursor`, and `configureFailPoint`. Thus, the rest of this
    // test won't need to create new connections and the number of open connections should remain
    // unchanged.
    const size_t kNumConnections = 4;
    std::vector<TaskExecutor::CallbackHandle> handles;
    auto cmd = BSON("find" << "test"
                           << "filter" << BSON("$where" << "sleep(100); return true;"));
    for (size_t i = 0; i < kNumConnections; i++) {
        handles.emplace_back(scheduleRemoteCommand(opCtx.get(), target, cmd));
    }
    for (const auto& cbHandle : handles) {
        executor()->wait(cbHandle);
    }

    int createdBefore, openBefore;
    {
        const auto fpName = "waitAfterCommandFinishesExecution";
        runRemoteCommand(opCtx.get(),
                         target,
                         BSON("configureFailPoint"
                              << fpName << "mode"
                              << "alwaysOn"
                              << "data"
                              << BSON("ns" << "test.test"
                                           << "commands" << BSON_ARRAY("getMore"))));
        ScopeGuard guard([&] {
            runRemoteCommand(opCtx.get(),
                             target,
                             BSON("configureFailPoint" << fpName << "mode"
                                                       << "off"));
        });

        RemoteCommandRequest rcr(target,
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 BSON("find" << "test"
                                             << "batchSize" << 10),
                                 opCtx.get());

        executor::TaskExecutorCursorOptions opts(
            /*pinConnection*/ false,  // Test relies on being in non-pinned mode
            /*batchSize*/ 10,
            /*preFetchNextBatch*/ true,
            /*planYieldPolicy*/ nullptr);

        auto tec = std::make_unique<TaskExecutorCursor>(executor(), rcr, std::move(opts));

        tec->populateCursor(opCtx.get());

        // Make sure there is enough time for a connection/stream to establish.
        assertConnectionStatsSoon(
            getFactory(),
            unittest::getFixtureConnectionString().getServers().front(),
            [](const ConnectionStatsPer& stats) { return stats.inUse >= 1; },
            [&](const GRPCConnectionStats& stats) { return stats.getTotalInUseStreams() >= 1; },
            "Timed out waiting for at least one in use connection.");

        // At least one of the connections is busy running the initial `getMore` command to populate
        // the cursor. The command is blocked on the remote host and does not return until after the
        // destructor for `tec` returns.
        auto beforeStats = getCreatedAndOpenConnectionStats(
            executor(), "NetworkInterfaceTL-" + kNetworkInterfaceInstanceName, target, usingGRPC);
        createdBefore = beforeStats.first;
        openBefore = beforeStats.second;
    }

    // Wait for all connections to become idle -- this ensures all tasks scheduled as part of
    // cleaning up `tec` have finished running.
    LOGV2(6531701, "Waiting for all connections to become idle");
    assertConnectionStatsSoon(
        getFactory(),
        unittest::getFixtureConnectionString().getServers().front(),
        [](const ConnectionStatsPer& stats) { return stats.inUse == 0; },
        [&](const GRPCConnectionStats& stats) { return stats.getTotalInUseStreams() == 0; },
        "Timed out waiting for connections to become idle.");

    auto [createdAfter, openAfter] = getCreatedAndOpenConnectionStats(
        executor(), "NetworkInterfaceTL-" + kNetworkInterfaceInstanceName, target, usingGRPC);

    // Check to see that our connection stats did not change.
    if (!usingGRPC) {
        ASSERT_EQ(createdBefore, createdAfter);
    }
    ASSERT_EQ(openBefore, openAfter);
}

}  // namespace
}  // namespace executor
}  // namespace mongo
