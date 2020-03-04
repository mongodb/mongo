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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/executor/task_executor.h"

#include "mongo/db/namespace_string.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace executor {
namespace {

class TaskExecutorFixture : public mongo::unittest::Test {
public:
    void setUp() override {
        std::shared_ptr<NetworkInterface> net = makeNetworkInterface("TaskExecutorTest");
        auto tp = std::make_unique<NetworkInterfaceThreadPool>(net.get());

        _executor = std::make_unique<ThreadPoolTaskExecutor>(std::move(tp), std::move(net));
        _executor->startup();
    };

    void tearDown() override {
        _executor->shutdown();
        _executor.reset();
    };

    TaskExecutor* executor() {
        return _executor.get();
    }

    bool waitUntilNoTasksOrDeadline(Date_t deadline) {
        while (Date_t::now() <= deadline) {
            if (!_executor->hasTasks()) {
                return true;
            }
        }

        return false;
    }

    ServiceContext::UniqueServiceContext _serviceCtx = ServiceContext::make();
    std::unique_ptr<ThreadPoolTaskExecutor> _executor;
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
        stdx::unique_lock<Latch> lk(_mutex);
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

    Mutex _mutex = MONGO_MAKE_LATCH("ExhaustRequestHandlerUtil::_mutex");
    stdx::condition_variable _cv;

    // called when a server sends a new isMaster exhaust response. Updates _responseOutcomeCount
    // and _replyUpdated.
    std::function<void(const executor::TaskExecutor::RemoteCommandCallbackArgs&)> _callbackFn =
        [&](const executor::TaskExecutor::RemoteCommandCallbackArgs& result) {
            {
                stdx::unique_lock<Latch> lk(_mutex);
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

TEST_F(TaskExecutorFixture, RunExhaustShouldReceiveMultipleResponses) {
    auto client = _serviceCtx->makeClient("TaskExecutorExhaustTest");
    auto opCtx = client->makeOperationContext();

    RemoteCommandRequest rcr(unittest::getFixtureConnectionString().getServers().front(),
                             "admin",
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
}

TEST_F(TaskExecutorFixture, RunExhaustShouldStopOnFailure) {
    // Turn on the failCommand failpoint for 'isMaster' on the server that we will schedule
    // 'isMaster' on below
    auto failCmdClient = _serviceCtx->makeClient("TaskExecutorExhaustTest");
    auto opCtx = failCmdClient->makeOperationContext();

    auto configureFailpointCmd = BSON("configureFailPoint"
                                      << "failCommand"
                                      << "mode"
                                      << "alwaysOn"
                                      << "data"
                                      << BSON("errorCode" << ErrorCodes::CommandFailed
                                                          << "failCommands"
                                                          << BSON_ARRAY("isMaster")));
    RemoteCommandRequest failCmd(unittest::getFixtureConnectionString().getServers().front(),
                                 "admin",
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
        auto stopFpCmd = BSON("configureFailPoint"
                              << "failCommand"
                              << "mode"
                              << "off");
        RemoteCommandRequest stopFpRequest(
            unittest::getFixtureConnectionString().getServers().front(),
            "admin",
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
        auto client = _serviceCtx->makeClient("TaskExecutorExhaustTest");
        auto opCtx = client->makeOperationContext();

        RemoteCommandRequest rcr(unittest::getFixtureConnectionString().getServers().front(),
                                 "admin",
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

        // The response should be marked as failed
        ASSERT_EQ(counters._success, 0);
        ASSERT_EQ(counters._failed, 1);

        // The tasks should be removed after 'isMaster' fails
        ASSERT_TRUE(waitUntilNoTasksOrDeadline(Date_t::now() + Seconds(5)));
    }
}

}  // namespace
}  // namespace executor
}  // namespace mongo
