/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/bson/oid.h"
#include "mongo/db/repl/hello/hello_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/executor/async_rpc_retry_policy.h"
#include "mongo/executor/async_rpc_targeter.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"

#include <memory>

namespace mongo::async_rpc {
using executor::NetworkInterface;
using executor::NetworkInterfaceMock;
using executor::NetworkTestEnv;
using executor::ThreadPoolMock;
using executor::ThreadPoolTaskExecutor;

class TestRetryPolicy : public RetryPolicy {
public:
    bool recordAndEvaluateRetry(Status s) final {
        if (_numRetriesPerformed == _maxRetries) {
            return false;
        }
        ++_numRetriesPerformed;
        return true;
    }

    Milliseconds getNextRetryDelay() final {
        auto&& out = _retryDelays.front();
        _retryDelays.pop_front();
        return out;
    }

    void pushRetryDelay(Milliseconds retryDelay) {
        _retryDelays.push_back(retryDelay);
    }

    BSONObj toBSON() const final {
        return BSON("retryPolicyType" << "TestRetryPolicy");
    }

    void setMaxNumRetries(int maxRetries) {
        _maxRetries = maxRetries;
    }

    int getNumRetriesPerformed() const {
        return _numRetriesPerformed;
    }

private:
    int _numRetriesPerformed, _maxRetries = 0;
    std::deque<Milliseconds> _retryDelays;
};

class AsyncRPCTestFixture : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _net = std::make_shared<NetworkInterfaceMock>();
        ThreadPoolMock::Options opts{};
        _executor = ThreadPoolTaskExecutor::create(
            std::make_unique<ThreadPoolMock>(_net.get(), 1, std::move(opts)), _net);
        _executor->startup();

        _networkTestEnv = std::make_shared<NetworkTestEnv>(getExecutorPtr().get(), _net.get());
    }

    void tearDown() override {
        _networkTestEnv.reset();
        // We must shutdown and join the executor to ensure there are no tasks running in the
        // background as we proceed with tearing down the test environment.
        _executor->shutdown();
        _executor->join();
        _executor.reset();
        _net.reset();
        ServiceContextTest::tearDown();
    }

    void onCommand(NetworkTestEnv::OnCommandFunction func) {
        _networkTestEnv->onCommand(func);
    }

    void onCommands(std::vector<NetworkTestEnv::OnCommandFunction> funcs) {
        _networkTestEnv->onCommands(funcs);
    }

    /**
     * Initialize an IDL command with the necessary fields (dbName) to avoid an invariant failure.
     */
    template <typename CommandType>
    void initializeCommand(CommandType& c) {
        c.setDbName(DatabaseName::createDatabaseName_forTest(boost::none, "testdb"));
    }

    /**
     * Generates an error BSONObj command response, using the given 'status' as
     * the error.
     */
    BSONObj createErrorResponse(Status status) {
        invariant(!status.isOK());
        BSONObjBuilder result;
        status.serializeErrorToBSON(&result);
        result.appendBool("ok", 0);
        return result.obj();
    }

    TaskExecutor& getExecutor() const {
        return *_executor;
    }

    std::shared_ptr<TaskExecutor> getExecutorPtr() const {
        return _executor;
    }

    NetworkInterfaceMock* getNetworkInterfaceMock() const {
        return _net.get();
    }

    void scheduleRequestAndAdvanceClockForRetry(std::shared_ptr<RetryPolicy> retryPolicy,
                                                NetworkTestEnv::OnCommandFunction onCommandFunc,
                                                Milliseconds advanceBy) {
        auto net = getNetworkInterfaceMock();
        onCommand(onCommandFunc);
        {
            executor::NetworkInterfaceMock::InNetworkGuard guard(net);
            net->advanceTime(net->now() + advanceBy);
        }
    }

protected:
    CancellationToken _cancellationToken{CancellationToken::uncancelable()};

private:
    unittest::MinimumLoggedSeverityGuard logSeverityGuardNetwork{
        logv2::LogComponent::kNetwork,
        logv2::LogSeverity::Debug(NetworkInterface::kDiagnosticLogLevel)};
    unittest::MinimumLoggedSeverityGuard logSeverityGuardExecutor{logv2::LogComponent::kExecutor,
                                                                  logv2::LogSeverity::Debug(3)};

    std::shared_ptr<NetworkTestEnv> _networkTestEnv;
    std::shared_ptr<TaskExecutor> _executor;
    std::shared_ptr<NetworkInterfaceMock> _net;
};

/**
 * Targeter for use in tests. Returns a user-configurable error when asked to resolve
 * targets; the error returned can be set when constructing this type.
 */
class FailingTargeter : public Targeter {
public:
    FailingTargeter(Status errorToFailWith) : _status{errorToFailWith} {}
    SemiFuture<std::vector<HostAndPort>> resolve(CancellationToken t) final {
        return _status;
    }

    SemiFuture<void> onRemoteCommandError(HostAndPort h, Status s) final {
        return SemiFuture<void>::makeReady();
    }

    Status getErrorStatus() {
        return _status;
    }

    Status _status;
};

class ShardIdTargeterForTest : public ShardIdTargeter {
public:
    ShardIdTargeterForTest(ExecutorPtr executor,
                           OperationContext* opCtx,
                           ShardId shardId,
                           ReadPreferenceSetting readPref,
                           std::vector<HostAndPort> resolvedHosts)
        : ShardIdTargeter(executor, opCtx, shardId, readPref) {
        _resolvedHosts = resolvedHosts;
    };

    SemiFuture<std::vector<HostAndPort>> resolve(CancellationToken t) final {
        return SemiFuture<std::vector<HostAndPort>>::makeReady(_resolvedHosts);
    }

    SemiFuture<void> onRemoteCommandError(HostAndPort h, Status s) final {
        return SemiFuture<void>::makeReady();
    }

private:
    std::vector<HostAndPort> _resolvedHosts;
};

class AsyncRPCTxnTestFixture : public AsyncRPCTestFixture {
public:
    void setUp() override {
        AsyncRPCTestFixture::setUp();
        _opCtxHolder = makeOperationContext();
        getOpCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
        _routerOpCtxSession.emplace(getOpCtx());
    }

    OperationContext* getOpCtx() {
        return _opCtxHolder.get();
    }

    void tearDown() override {
        AsyncRPCTestFixture::tearDown();
    }

    const LogicalSessionId& getSessionId() {
        return *getOpCtx()->getLogicalSessionId();
    }

private:
    ServiceContext::UniqueOperationContext _opCtxHolder;
    boost::optional<mongo::RouterOperationContextSession> _routerOpCtxSession;
};

}  // namespace mongo::async_rpc
