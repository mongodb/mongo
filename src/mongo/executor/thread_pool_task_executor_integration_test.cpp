/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/executor_integration_test_connection_stats.h"
#include "mongo/executor/executor_integration_test_fixture.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/network_interface_tl.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/grpc_connection_stats_gen.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace executor {
namespace {
constexpr auto kNetworkInterfaceInstanceName = "TaskExecutorTest"_sd;
constexpr auto kNetworkInterfaceTestInstanceName = "TaskExecutorTestSetup"_sd;
constexpr auto kNetworkInterfaceGRPCInstanceName = "FixtureNet"_sd;

class TaskExecutorFixture : public ExecutorIntegrationTestFixture {
public:
    TaskExecutorFixture() = default;

    void setUp() override {
        _executor = makeExecutor(kNetworkInterfaceInstanceName);
        _executor->startup();

        _testExecutor = makeExecutor(kNetworkInterfaceTestInstanceName);
        _testExecutor->startup();
    };

    std::shared_ptr<ThreadPoolTaskExecutor> makeExecutor(StringData name) {
        std::shared_ptr<NetworkInterface> net;
        if (!unittest::shouldUseGRPCEgress()) {
            ConnectionPool::Options cpOptions{};
            cpOptions.minConnections = 0;
            net = makeNetworkInterface(std::string{name}, nullptr, nullptr, std::move(cpOptions));
        } else {
#ifdef MONGO_CONFIG_GRPC
            net = makeNetworkInterfaceGRPC(kNetworkInterfaceGRPCInstanceName);
#else
            MONGO_UNREACHABLE;
#endif
        }

        ThreadPool::Options tpOptions;
        tpOptions.threadNamePrefix = "TaskExecutorTestThreadPool-";
        tpOptions.poolName = "TaskExecutorTestThreadPool";
        tpOptions.maxThreads = 4;

        return ThreadPoolTaskExecutor::create(std::make_unique<ThreadPool>(tpOptions),
                                              std::move(net));
    }

    void tearDown() override {
        _executor->shutdown();
        _executor.reset();
    };

    TaskExecutor* executor() {
        return _executor.get();
    }

    TaskExecutor* setupExecutor() {
        return _testExecutor.get();
    }

    BSONObj runSetupCommandSync(const DatabaseName& db, BSONObj cmdObj) override {
        auto pf = makePromiseFuture<RemoteCommandResponse>();
        auto res = assertOK(
            setupExecutor()
                ->scheduleRemoteCommand(RemoteCommandRequest(getServer(), db, cmdObj, nullptr),
                                        CancellationToken::uncancelable())
                .getNoThrow());
        return res.data;
    }

    bool waitUntilNoTasksOrDeadline(Date_t deadline) {
        while (Date_t::now() <= deadline) {
            if (!_executor->hasTasks()) {
                return true;
            }
        }

        return false;
    }

    const AsyncClientFactory& getFactory() {
        return checked_pointer_cast<NetworkInterfaceTL>(_executor->getNetworkInterface())
            ->getClientFactory_forTest();
    }

    std::shared_ptr<ThreadPoolTaskExecutor> _executor;

private:
    std::shared_ptr<ThreadPoolTaskExecutor> _testExecutor;
};

class RequestHandlerUtil {
public:
    struct responseOutcomeCount {
        int _success = 0;
        int _failed = 0;
    };

    std::function<void(const executor::TaskExecutor::RemoteCommandCallbackArgs&)>&&
    getRequestCallbackFn() {
        return std::move(_callbackFn);
    }

    RequestHandlerUtil::responseOutcomeCount getCountersWhenReady() {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _cv.wait(_mutex, [&] { return _replyUpdated; });
        _replyUpdated = false;
        return _responseOutcomeCount;
    }

private:
    // set to true once '_responseOutcomeCount' has been updated. Used to indicate that a new
    // response has been sent.
    bool _replyUpdated = false;

    // counter of how many successful and failed responses were received.
    responseOutcomeCount _responseOutcomeCount;

    stdx::mutex _mutex;
    stdx::condition_variable _cv;

    // called when a server sends a new isMaster exhaust response. Updates _responseOutcomeCount
    // and _replyUpdated.
    std::function<void(const executor::TaskExecutor::RemoteCommandCallbackArgs&)> _callbackFn =
        [&](const executor::TaskExecutor::RemoteCommandCallbackArgs& result) {
            {
                stdx::unique_lock<stdx::mutex> lk(_mutex);
                if (result.response.isOK()) {
                    _responseOutcomeCount._success++;
                } else {
                    _responseOutcomeCount._failed++;
                }
                _replyUpdated = true;
            }

            _cv.notify_all();
        };
};

TEST_F(TaskExecutorFixture, ScheduleWorkAt) {
    auto start = executor()->now();
    auto target = start + Milliseconds(50);
    auto pf = makePromiseFuture<void>();
    auto swCbh =
        executor()->scheduleWorkAt(target, [&](auto args) { pf.promise.setFrom(args.status); });
    ASSERT_OK(swCbh);
    ASSERT_OK(pf.future.getNoThrow());
    ASSERT_GTE(executor()->now(), target);
}

TEST_F(TaskExecutorFixture, ScheduleWorkAtCancel) {
    auto client = getGlobalServiceContext()->getService()->makeClient("ScheduleWorkAtCancel");
    auto opCtx = client->makeOperationContext();

    auto start = executor()->now();
    auto pf = makePromiseFuture<void>();
    auto swCbh = executor()->scheduleWorkAt(start + Seconds(60),
                                            [&](auto args) { pf.promise.setFrom(args.status); });
    ASSERT_OK(swCbh);
    ASSERT_FALSE(pf.future.isReady());
    executor()->cancel(swCbh.getValue());
    opCtx->setDeadlineAfterNowBy(Seconds(30), ErrorCodes::ExceededTimeLimit);
    ASSERT_EQ(pf.future.getNoThrow(opCtx.get()), ErrorCodes::CallbackCanceled);
}

TEST_F(TaskExecutorFixture, Shutdown) {
    auto client = getGlobalServiceContext()->getService()->makeClient("TaskExecutorShutdown");
    auto opCtx = client->makeOperationContext();

    auto fp = configureFailCommand("ping", {}, Milliseconds(60000));

    auto clientOpKey = UUID::gen();
    auto req = makeTestCommand(
        RemoteCommandRequest::kNoTimeout, BSON("ping" << 1), nullptr, false, {}, clientOpKey);
    auto pf = makePromiseFuture<RemoteCommandResponse>();
    auto swCbHandle =
        executor()->scheduleRemoteCommand(req, [&](TaskExecutor::RemoteCommandCallbackArgs args) {
            pf.promise.emplaceValue(args.response);
        });
    ASSERT_OK(swCbHandle);
    ON_BLOCK_EXIT([&] { killOp(clientOpKey); });

    auto exhPf = makePromiseFuture<void>();
    auto exhaustReq =
        makeTestCommand(RemoteCommandRequest::kNoTimeout,
                        BSON("hello" << 1 << "maxAwaitTimeMS" << 100 << "topologyVersion"
                                     << TopologyVersion(OID::max(), 0).toBSON()));
    auto swExCbh = executor()->scheduleExhaustRemoteCommand(exhaustReq, [&](auto args) {
        if (!args.response.isOK()) {
            exhPf.promise.setFrom(args.response.status);
        }
    });
    ASSERT_OK(swExCbh);

    fp.waitForAdditionalTimesEntered(1);
    ASSERT_FALSE(pf.future.isReady());
    ASSERT_FALSE(exhPf.future.isReady());

    executor()->shutdown();
    executor()->join();

    ASSERT_TRUE(pf.future.isReady());
    ASSERT_EQ(pf.future.get().status, ErrorCodes::CallbackCanceled);
    ASSERT_EQ(exhPf.future.getNoThrow(), ErrorCodes::CallbackCanceled);

    assertConnectionStatsSoon(
        getFactory(),
        getServer(),
        [](const ConnectionStatsPer& stats) {
            return stats.inUse + stats.available + stats.leased == 0;
        },
        [&](const GRPCConnectionStats& stats) {
            return (stats.getTotalInUseStreams() + stats.getTotalLeasedStreams() +
                    stats.getTotalOpenChannels()) == 0;
        },
        "Connection pools should be drained after shutdown + join");
}

TEST_F(TaskExecutorFixture, ScheduleRemoteCommand) {
    auto client = getGlobalServiceContext()->getService()->makeClient("TaskExecutorExhaustTest");
    auto opCtx = client->makeOperationContext();

    RemoteCommandRequest rcr(unittest::getFixtureConnectionString().getServers().front(),
                             DatabaseName::kAdmin,
                             BSON("ping" << 1),
                             nullptr);

    auto pf = makePromiseFuture<RemoteCommandResponse>();
    auto swCbHandle = executor()->scheduleRemoteCommand(
        std::move(rcr), [&](TaskExecutor::RemoteCommandCallbackArgs args) {
            pf.promise.emplaceValue(args.response);
        });
    ASSERT_OK(swCbHandle.getStatus());
    auto cbHandle = swCbHandle.getValue();

    opCtx->setDeadlineAfterNowBy(Seconds(30), ErrorCodes::ExceededTimeLimit);
    auto resp = pf.future.get(opCtx.get());
    ASSERT_OK(resp.status);
    ASSERT_OK(getStatusFromCommandResult(resp.data));
}

TEST_F(TaskExecutorFixture, ScheduleRemoteCommandCancel) {
    auto client = getGlobalServiceContext()->getService()->makeClient("TaskExecutorExhaustTest");
    auto opCtx = client->makeOperationContext();
    auto opKey = UUID::gen();

    auto fpGuard = configureFailCommand("ping", {}, Milliseconds(60000));

    auto pf = makePromiseFuture<RemoteCommandResponse>();
    auto request = makeTestCommand(
        RemoteCommandRequest::kNoTimeout, BSON("ping" << 1), nullptr, false, {}, opKey);
    auto swCbh = executor()->scheduleRemoteCommand(
        request, [&](TaskExecutor::RemoteCommandCallbackArgs args) {
            pf.promise.emplaceValue(args.response);
        });
    ASSERT_OK(swCbh);
    ON_BLOCK_EXIT([&] { killOp(opKey); });

    fpGuard.waitForAdditionalTimesEntered(1);
    ASSERT_FALSE(pf.future.isReady());
    executor()->cancel(swCbh.getValue());

    opCtx->setDeadlineAfterNowBy(Seconds(30), ErrorCodes::ExceededTimeLimit);
    auto resp = pf.future.get(opCtx.get());
    ASSERT_EQ(resp.status, ErrorCodes::CallbackCanceled);
}

TEST_F(TaskExecutorFixture, RunExhaustShouldReceiveMultipleResponses) {
    auto client = getGlobalServiceContext()->getService()->makeClient("TaskExecutorExhaustTest");
    auto opCtx = client->makeOperationContext();

    RemoteCommandRequest rcr(unittest::getFixtureConnectionString().getServers().front(),
                             DatabaseName::kAdmin,
                             BSON("isMaster" << 1 << "maxAwaitTimeMS" << 1000 << "topologyVersion"
                                             << TopologyVersion(OID::max(), 0).toBSON()),
                             opCtx.get());

    RequestHandlerUtil exhaustRequestHandler;
    auto swCbHandle = executor()->scheduleExhaustRemoteCommand(
        std::move(rcr), exhaustRequestHandler.getRequestCallbackFn());
    ASSERT_OK(swCbHandle.getStatus());
    auto cbHandle = swCbHandle.getValue();

    {
        auto counters = exhaustRequestHandler.getCountersWhenReady();
        ASSERT(cbHandle.isValid());

        // The first response should be successful
        ASSERT_EQ(counters._success, 1);
        ASSERT_EQ(counters._failed, 0);
    }

    {
        auto counters = exhaustRequestHandler.getCountersWhenReady();
        ASSERT(cbHandle.isValid());

        // The second response should also be successful
        ASSERT_EQ(counters._success, 2);
        ASSERT_EQ(counters._failed, 0);
    }

    // Cancel the callback
    ASSERT(cbHandle.isValid());
    executor()->cancel(cbHandle);
    ASSERT(cbHandle.isCanceled());

    // The tasks should be removed after 'isMaster' fails
    ASSERT_TRUE(waitUntilNoTasksOrDeadline(Date_t::now() + Seconds(5)));

    assertConnectionStatsSoon(
        getFactory(),
        getServer(),
        [](const ConnectionStatsPer& stats) {
            return stats.inUse + stats.available + stats.leased == 0;
        },
        [&](const GRPCConnectionStats& stats) {
            return stats.getTotalInUseStreams() + stats.getTotalLeasedStreams() == 0;
        },
        "Connection should be discarded after exhaust cancel");
}

TEST_F(TaskExecutorFixture, RunExhaustFutureShouldReceiveMultipleResponses) {
    auto client = getGlobalServiceContext()->getService()->makeClient("TaskExecutorExhaustTest");
    auto opCtx = client->makeOperationContext();

    RemoteCommandRequest rcr(unittest::getFixtureConnectionString().getServers().front(),
                             DatabaseName::kAdmin,
                             BSON("isMaster" << 1 << "maxAwaitTimeMS" << 1000 << "topologyVersion"
                                             << TopologyVersion(OID::max(), 0).toBSON()),
                             opCtx.get());

    RequestHandlerUtil exhaustRequestHandler;
    auto swFuture =
        executor()->scheduleExhaustRemoteCommand(std::move(rcr),
                                                 exhaustRequestHandler.getRequestCallbackFn(),
                                                 opCtx->getCancellationToken());

    for (size_t i = 0; i < 5; ++i) {
        auto counters = exhaustRequestHandler.getCountersWhenReady();

        // Each response should be successful
        ASSERT_EQ(counters._success, i + 1);
        ASSERT_EQ(counters._failed, 0);
        ASSERT_FALSE(swFuture.isReady());
    }

    // Cancel the callback
    opCtx->markKilled();

    // The tasks should be removed after 'isMaster' fails
    ASSERT_TRUE(waitUntilNoTasksOrDeadline(Date_t::now() + Seconds(5)));
    ASSERT_FALSE(swFuture.getNoThrow().isOK());

    assertConnectionStatsSoon(
        getFactory(),
        getServer(),
        [](const ConnectionStatsPer& stats) {
            return stats.inUse + stats.available + stats.leased == 0;
        },
        [&](const GRPCConnectionStats& stats) {
            return stats.getTotalInUseStreams() + stats.getTotalLeasedStreams() == 0;
        },
        "Connection should be discarded after exhaust cancel");
}

TEST_F(TaskExecutorFixture, RunExhaustShouldStopOnFailure) {
    // Turn on the failCommand failpoint for 'isMaster' on the server that we will schedule
    // 'isMaster' on below
    auto failCmdClient =
        getGlobalServiceContext()->getService()->makeClient("TaskExecutorExhaustTest");
    auto opCtx = failCmdClient->makeOperationContext();

    auto configureFailpointCmd =
        BSON("configureFailPoint" << "failCommand"
                                  << "mode"
                                  << "alwaysOn"
                                  << "data"
                                  << BSON("errorCode" << ErrorCodes::CommandFailed << "failCommands"
                                                      << BSON_ARRAY("isMaster")));
    RemoteCommandRequest failCmd(unittest::getFixtureConnectionString().getServers().front(),
                                 DatabaseName::kAdmin,
                                 configureFailpointCmd,
                                 opCtx.get());
    RequestHandlerUtil failCmdRequestHandler;
    auto swCbHandle = _executor->scheduleRemoteCommand(
        std::move(failCmd), failCmdRequestHandler.getRequestCallbackFn());
    ASSERT_OK(swCbHandle.getStatus());
    auto cbHandle = swCbHandle.getValue();

    // Assert 'configureFailPoint' was successful
    auto counters = failCmdRequestHandler.getCountersWhenReady();
    ASSERT(cbHandle.isValid());
    ASSERT_EQ(counters._success, 1);
    ASSERT_EQ(counters._failed, 0);

    ON_BLOCK_EXIT([&] {
        auto stopFpCmd = BSON("configureFailPoint" << "failCommand"
                                                   << "mode"
                                                   << "off");
        RemoteCommandRequest stopFpRequest(
            unittest::getFixtureConnectionString().getServers().front(),
            DatabaseName::kAdmin,
            stopFpCmd,
            opCtx.get());
        auto swCbHandle = _executor->scheduleRemoteCommand(
            std::move(stopFpRequest), failCmdRequestHandler.getRequestCallbackFn());

        // Assert the failpoint is correctly turned off
        auto counters = failCmdRequestHandler.getCountersWhenReady();
        ASSERT(cbHandle.isValid());

        ASSERT_EQ(counters._success, 2);
        ASSERT_EQ(counters._failed, 0);
    });

    {
        auto client =
            getGlobalServiceContext()->getService()->makeClient("TaskExecutorExhaustTest");
        auto opCtx = client->makeOperationContext();

        RemoteCommandRequest rcr(unittest::getFixtureConnectionString().getServers().front(),
                                 DatabaseName::kAdmin,
                                 BSON("isMaster" << 1 << "maxAwaitTimeMS" << 1000
                                                 << "topologyVersion"
                                                 << TopologyVersion(OID::max(), 0).toBSON()),
                                 opCtx.get());

        RequestHandlerUtil exhaustRequestHandler;
        auto swCbHandle = executor()->scheduleExhaustRemoteCommand(
            std::move(rcr), exhaustRequestHandler.getRequestCallbackFn());
        ASSERT_OK(swCbHandle.getStatus());
        auto cbHandle = swCbHandle.getValue();

        auto counters = exhaustRequestHandler.getCountersWhenReady();

        // The response should be marked as succeeded, since in terms of networking it was
        // successful.
        ASSERT_EQ(counters._success, 1);
        ASSERT_EQ(counters._failed, 0);

        // The tasks should be removed after 'isMaster' fails
        ASSERT_TRUE(waitUntilNoTasksOrDeadline(Date_t::now() + Seconds(5)));

        assertConnectionStatsSoon(
            getFactory(),
            getServer(),
            [](const ConnectionStatsPer& stats) {
                return stats.inUse == 0 && stats.available == 1 && stats.leased == 0;
            },
            [&](const GRPCConnectionStats& stats) {
                return stats.getTotalInUseStreams() + stats.getTotalLeasedStreams() == 0;
            },
            "Connection should be returned to the pool after exhaust command completion");
    }
}

/**
 * This test simulates the case where exhaust responses are produced faster than they can be
 * processed by the TaskExecutor. It does so via the following steps:
 *
 *   - Insert documents with _id in [1, n]
 *   - Opens an exhaust cursor in ascending _id order
 *   - Upon receiving response(s), blocks in the thread executing the callback until the server has
 *     fully exhausted the cursor.
 *   - Pushes responses into a queue as they are received.
 *
 * We should expect to see all of the responses processed in order.
 */
TEST_F(TaskExecutorFixture, FastExhaustResponses) {
    // This test assumes that the TCP buffer can fit ~100 bytes.
    // If that isn't the case, the test will fail due to the cursor never becoming fully exhausted,
    // as the server-side will block indefinitely when trying to send a response once the buffer has
    // filled.
    constexpr size_t kNumResponses = 10;

    auto client = getGlobalServiceContext()->getService()->makeClient("FastExhaustResponsesTest");
    auto opCtx = client->makeOperationContext();

    std::vector<BSONObj> documents;
    for (size_t x = 0; x < kNumResponses; x++) {
        documents.push_back(BSON("_id" << std::int32_t(x)));
    }

    const auto nss =
        NamespaceStringUtil::deserialize(DatabaseName::kMdbTesting, "FastExhaustResponses");

    runSetupCommandSync(nss.dbName(), BSON("drop" << nss.coll()));

    write_ops::InsertCommandRequest insert(nss);
    insert.setDocuments(documents);
    runSetupCommandSync(nss.dbName(), insert.toBSON());

    FindCommandRequest find(nss);
    find.setSort(BSON("_id" << 1));
    find.setBatchSize(0);
    auto findReq = RemoteCommandRequest(getServer(), nss.dbName(), find.toBSON(), opCtx.get());
    auto cursorReply = [&]() {
        auto resp =
            executor()->scheduleRemoteCommand(findReq, CancellationToken::uncancelable()).get();
        ASSERT_OK(resp.status);
        ASSERT_OK(getStatusFromCommandResult(resp.data));
        return CursorInitialReply::parseOwned(std::move(resp.data), IDLParserContext("findReply"));
    }();

    Notification<void> cursorOpened;
    Notification<void> cursorExhausted;

    Atomic<bool> firstResponse = true;
    SingleProducerSingleConsumerQueue<RemoteCommandResponse> queue;

    auto cursorId = cursorReply.getCursor()->getCursorId();
    GetMoreCommandRequest getMore(cursorId, std::string{nss.coll()});
    getMore.setDbName(nss.dbName());
    getMore.setBatchSize(1);
    RemoteCommandRequest getMoreRequest(getServer(), nss.dbName(), getMore.toBSON(), opCtx.get());
    auto swCbHandle = executor()->scheduleExhaustRemoteCommand(
        getMoreRequest, [&](TaskExecutor::RemoteCommandCallbackArgs args) {
            if (firstResponse.swap(false)) {
                cursorOpened.set();
            }
            cursorExhausted.get();
            auto shouldClose = !args.response.moreToCome;
            queue.push(std::move(args.response));

            if (shouldClose) {
                queue.closeProducerEnd();
            }
        });
    ASSERT_OK(swCbHandle);
    ON_BLOCK_EXIT([&] {
        executor()->cancel(swCbHandle.getValue());
        executor()->wait(swCbHandle.getValue());
    });

    cursorOpened.waitFor(opCtx.get(), Seconds(60));
    while (true) {
        AggregateCommandRequest agg(
            NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin));
        agg.setPipeline({BSON("$currentOp" << BSON("idleCursors" << true)),
                         BSON("$match" << BSON("cursor.cursorId" << cursorId))});
        auto out = runSetupCommandSync(DatabaseName::kAdmin, agg.toBSON());
        auto aggReply = CursorInitialReply::parse(out, IDLParserContext("aggReply"));

        // Once cursor is exhausted, signal to handler thread it can start processing responses.
        if (!aggReply.getCursor() || aggReply.getCursor()->getFirstBatch().empty()) {
            LOGV2(9311409, "Cursor exhausted, unblocking TaskExecutor threads");
            cursorExhausted.set();
            break;
        }
    }

    size_t nReceived = 0;
    opCtx->setDeadlineAfterNowBy(Seconds(120), ErrorCodes::ExceededTimeLimit);
    while (queue.waitForNonEmptyNoThrow(opCtx.get()).isOK()) {
        auto resp = queue.pop();
        ASSERT_OK(resp.status);

        auto parsed = CursorGetMoreReply::parse(resp.data, IDLParserContext("CursorGetMoreReply"));
        auto batch = parsed.getCursor().getNextBatch();

        ASSERT_LTE(batch.size(), 1);
        nReceived += batch.size();
        ASSERT_LTE(nReceived, documents.size());

        if (nReceived < documents.size()) {
            ASSERT_TRUE(resp.moreToCome);
        }

        if (batch.empty()) {
            // Exhaust command should be complete once we receive an empty batch.
            executor()->wait(swCbHandle.getValue(), opCtx.get());
        } else {
            auto& expected = documents[nReceived - 1];
            ASSERT_BSONOBJ_EQ(batch[0], expected);
        }
    }

    ASSERT_EQ(nReceived, documents.size());
}

}  // namespace
}  // namespace executor
}  // namespace mongo
