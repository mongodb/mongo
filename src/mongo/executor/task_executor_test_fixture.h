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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"

#include <memory>

namespace MONGO_MOD_PUB mongo {
namespace executor {

class TaskExecutor;
class NetworkInterface;
class NetworkInterfaceMock;

/**
 * Test fixture for tests that require a TaskExecutor backed by a NetworkInterfaceMock.
 */
class TaskExecutorTest : public unittest::Test {
public:
    /**
     * Creates an initial error status suitable for checking if
     * component has modified the 'status' field in test fixture.
     */
    static Status getDetectableErrorStatus();

    /**
     * Validates command name in remote command request. Returns the remote command request from
     * the network interface for further validation if the command name matches.
     */
    static RemoteCommandRequest assertRemoteCommandNameEquals(StringData cmdName,
                                                              const RemoteCommandRequest& request);

    ~TaskExecutorTest() override;

    executor::NetworkInterfaceMock* getNet() {
        return _net;
    }

    void runReadyNetworkOperations();

    TaskExecutor& getExecutor() {
        return *_executor;
    }

    std::shared_ptr<TaskExecutor> getExecutorPtr() const {
        return _executor;
    }

    /**
     * Initializes both the NetworkInterfaceMock and TaskExecutor but does not start the executor.
     */
    void setUp() override;
    void tearDown() override;

    void launchExecutorThread();
    void shutdownExecutorThread();
    void joinExecutorThread();

private:
    /**
     * Unused implementation of test function. This allows us to instantiate
     * TaskExecutorTest on its own without the need to inherit from it in a test.
     * This supports using TaskExecutorTest inside another test fixture and works around the
     * limitation that tests cannot inherit from multiple test fixtures.
     *
     * It is an error to call this implementation of _doTest() directly.
     */
    void _doTest() override;

    virtual std::shared_ptr<TaskExecutor> makeTaskExecutor(
        std::unique_ptr<NetworkInterfaceMock> net) = 0;

    virtual void postExecutorThreadLaunch();

    NetworkInterfaceMock* _net;
    std::shared_ptr<TaskExecutor> _executor;
    bool _needsShutDown{false};

    unittest::MinimumLoggedSeverityGuard logSeverityGuardNetwork{
        logv2::LogComponent::kNetwork,
        logv2::LogSeverity::Debug(NetworkInterface::kDiagnosticLogLevel)};
    unittest::MinimumLoggedSeverityGuard logSeverityGuardExecutor{logv2::LogComponent::kExecutor,
                                                                  logv2::LogSeverity::Debug(3)};
};

}  // namespace executor
}  // namespace MONGO_MOD_PUB mongo
