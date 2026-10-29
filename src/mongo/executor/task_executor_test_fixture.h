// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {
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
    static RemoteCommandRequest assertRemoteCommandNameEquals(std::string_view cmdName,
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
}  // namespace mongo
