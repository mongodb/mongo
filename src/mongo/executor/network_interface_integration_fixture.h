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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
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
#include "mongo/stdx/mutex.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"

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

class NetworkInterfaceIntegrationFixture : public ExecutorIntegrationTestFixture {
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
        auto lk = stdx::unique_lock(_mutex);
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
    mutable stdx::mutex _mutex;

    boost::optional<ConnectionPool::Options> _opts;
};

}  // namespace executor
}  // namespace mongo
