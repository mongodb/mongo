// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/task_executor_test_fixture.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace mongo {
namespace executor {

Status TaskExecutorTest::getDetectableErrorStatus() {
    return Status(ErrorCodes::InternalError, "Not mutated");
}

RemoteCommandRequest TaskExecutorTest::assertRemoteCommandNameEquals(
    std::string_view cmdName, const RemoteCommandRequest& request) {
    auto&& cmdObj = request.cmdObj;
    ASSERT_FALSE(cmdObj.isEmpty());
    if (cmdName != cmdObj.firstElementFieldName()) {
        std::string msg = str::stream()
            << "Expected command name \"" << cmdName << "\" in remote command request but found \""
            << cmdObj.firstElementFieldName() << "\" instead: " << request.toString();
        FAIL(msg);
    }
    return request;
}

TaskExecutorTest::~TaskExecutorTest() = default;

void TaskExecutorTest::setUp() {
    auto net = std::make_unique<NetworkInterfaceMock>();
    _net = net.get();
    _executor = makeTaskExecutor(std::move(net));
}

void TaskExecutorTest::tearDown() {
    shutdownExecutorThread();
    joinExecutorThread();
    _executor.reset();
    _net = nullptr;
}

void TaskExecutorTest::runReadyNetworkOperations() {
    executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
    getNet()->runReadyNetworkOperations();
}

void TaskExecutorTest::launchExecutorThread() {
    _executor->startup();
    _needsShutDown = true;
    postExecutorThreadLaunch();
}

void TaskExecutorTest::shutdownExecutorThread() {
    if (!_needsShutDown) {
        return;
    }

    _executor->shutdown();
    runReadyNetworkOperations();
}

void TaskExecutorTest::joinExecutorThread() {
    _net->enterNetwork();
    _net->drainUnfinishedNetworkOperations();
    _net->exitNetwork();
    _executor->join();
}

void TaskExecutorTest::postExecutorThreadLaunch() {}

}  // namespace executor
}  // namespace mongo
