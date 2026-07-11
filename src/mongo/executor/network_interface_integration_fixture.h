// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/baton.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/executor_integration_test_fixture.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>

#include <boost/optional.hpp>

namespace mongo {

class PseudoRandom;

namespace executor {

/**
 * A mock class mimicking TaskExecutor::CallbackState, does nothing.
 */
class MockCallbackState final : public TaskExecutor::CallbackState {
public:
    MockCallbackState() = default;
    void cancel() override {}
    void waitForCompletion() override {}
    bool isCanceled() const override {
        return false;
    }
};

inline TaskExecutor::CallbackHandle makeCallbackHandle() {
    return TaskExecutor::CallbackHandle(std::make_shared<MockCallbackState>());
}

using StartCommandCB = std::function<void(const RemoteCommandResponse&)>;

class [[MONGO_MOD_OPEN]] NetworkInterfaceIntegrationFixture
    : public ExecutorIntegrationTestFixture {
public:
    void createNet();
    void startNet();
    void tearDown() override;

    NetworkInterface& net();

    /**
     * A NetworkInterface used for test fixture needs (e.g. failpoints).
     */
    NetworkInterface& fixtureNet();

    ConnectionString fixture();

    void setRandomNumberGenerator(PseudoRandom* generator);

    void resetIsInternalClient(bool isInternalClient);

    PseudoRandom* getRandomNumberGenerator();

    ConnectionPool::Options makeDefaultConnectionPoolOptions();

    /**
     * Runs a command, returning a future representing its response. When waiting on this future,
     * use the interruptible returned by interruptible() and only do so from one thread.
     */
    Future<RemoteCommandResponse> runCommand(
        const TaskExecutor::CallbackHandle& cbHandle,
        RemoteCommandRequest request,
        const CancellationToken& token = CancellationToken::uncancelable());

    void cancelCommand(const TaskExecutor::CallbackHandle& cbHandle);

    /**
     * Runs a command on the fixture NetworkInterface and asserts it suceeded.
     */
    BSONObj runSetupCommandSync(const DatabaseName& db, BSONObj cmdObj) override;

    /**
     * Runs a command synchronously, returning its response. While this executes, no other thread
     * may use interruptible().
     */
    RemoteCommandResponse runCommandSync(RemoteCommandRequest& request);

    /**
     * Asserts that a command succeeds or fails in some disposition. While these execute, no other
     * thread may use interruptible().
     */
    void assertCommandOK(const DatabaseName& db,
                         const BSONObj& cmd,
                         Milliseconds timeoutMillis = Minutes(5),
                         transport::ConnectSSLMode sslMode = transport::kGlobalSSLMode);
    void assertCommandFailsOnClient(const DatabaseName& db,
                                    const BSONObj& cmd,
                                    ErrorCodes::Error reason,
                                    Milliseconds timeoutMillis = Minutes(5));

    void assertCommandFailsOnServer(const DatabaseName& db,
                                    const BSONObj& cmd,
                                    ErrorCodes::Error reason,
                                    Milliseconds timeoutMillis = Minutes(5));

    void assertWriteError(const DatabaseName& db,
                          const BSONObj& cmd,
                          ErrorCodes::Error reason,
                          Milliseconds timeoutMillis = Minutes(5));

    size_t getInProgress() {
        auto lk = std::unique_lock(_mutex);
        return _workInProgress;
    }

    /**
     * Returns a Baton that can be used to run commands on, or nullptr for reactor-only operation.
     * Implicitly used by runCommand, cancelCommand, runCommandSync, and assertCommand* variants.
     */
    virtual BatonHandle baton() {
        return nullptr;
    }

    /** Returns an Interruptible appropriate for the Baton returned from baton(). */
    virtual Interruptible* interruptible() {
        return Interruptible::notInterruptible();
    }

    template <typename FutureType>
    auto getWithTimeout(FutureType& future, Interruptible& interruptible, Milliseconds timeout) {
        auto deadline = getGlobalServiceContext()->getFastClockSource()->now() + timeout;
        auto guard = interruptible.makeDeadlineGuard(deadline, ErrorCodes::ExceededTimeLimit);
        return future.get(&interruptible);
    }

    void setConnectionPoolOptions(const ConnectionPool::Options& opts) {
        _opts = opts;
    }

protected:
    virtual std::unique_ptr<NetworkInterface> _makeNet(std::string instanceName,
                                                       transport::TransportProtocol protocol);

private:
    void _onSchedulingCommand();
    void _onCompletingCommand();

    std::unique_ptr<NetworkInterface> _fixtureNet;
    std::unique_ptr<NetworkInterface> _net;
    PseudoRandom* _rng = nullptr;

    size_t _workInProgress = 0;
    stdx::condition_variable _fixtureIsIdle;
    mutable std::mutex _mutex;

    boost::optional<ConnectionPool::Options> _opts;
};

}  // namespace executor
}  // namespace mongo
