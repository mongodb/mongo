// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/oid.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/repl/hello/hello_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/executor/async_rpc.h"
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

class TestRetryStrategy final : public mongo::RetryStrategy {
public:
    bool recordFailureAndEvaluateShouldRetry(Status s,
                                             const boost::optional<HostAndPort>& target,
                                             std::span<const std::string> errorLabels,
                                             boost::optional<Milliseconds> baseBackoffMS) override {
        // Record the most recent server-hinted retry delay so tests can assert that async_rpc
        // extracted it from the remote response and passed it through to the strategy.
        _lastBaseBackoffMS = baseBackoffMS;

        if (_numRetriesPerformed == _maxRetries) {
            return false;
        }
        ++_numRetriesPerformed;

        // Pop and save the next retry delay when we decide to retry
        if (!_retryDelays.empty()) {
            _nextRetryDelay = _retryDelays.front();
            _retryDelays.pop_front();
        }

        return true;
    }

    Milliseconds getNextRetryDelay() const override {
        return _nextRetryDelay;
    }

    void recordSuccess(const boost::optional<HostAndPort>& target) override {
        // Noop, as there's nothing to cleanup on success.
    }

    void recordBackoff(Milliseconds backoff) override {
        _totalBackoff += backoff;
    }

    const TargetingMetadata& getTargetingMetadata() const override {
        static const TargetingMetadata emptyMetadata{};
        return emptyMetadata;
    }

    void pushRetryDelay(Milliseconds retryDelay) {
        _retryDelays.push_back(retryDelay);
    }

    void setMaxNumRetries(int maxRetries) {
        _maxRetries = maxRetries;
    }

    int getNumRetriesPerformed() const {
        return _numRetriesPerformed;
    }

    Milliseconds getTotalBackoff() const {
        return _totalBackoff;
    }

    boost::optional<Milliseconds> getLastBaseBackoffMS() const {
        return _lastBaseBackoffMS;
    }

private:
    int _numRetriesPerformed, _maxRetries = 0;
    Milliseconds _nextRetryDelay{0};  // Current retry delay
    std::deque<Milliseconds> _retryDelays;
    Milliseconds _totalBackoff;
    boost::optional<Milliseconds> _lastBaseBackoffMS;
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

    /**
     * Generates a retryable error BSONObj command response with the 'SystemOverloaded'
     * error label.
     */
    BSONObj createErrorSystemOverloaded(ErrorCodes::Error errorCode) {
        BSONObjBuilder bob;
        bob.append("ok", 0.0);
        bob.append("code", errorCode);
        bob.append("errmsg", "overloaded");
        bob.append("codeName", ErrorCodes::errorString(errorCode));
        {
            BSONArrayBuilder arrayBuilder = bob.subarrayStart(kErrorLabelsFieldName);
            arrayBuilder.append(ErrorLabel::kSystemOverloadedError);
            arrayBuilder.append(ErrorLabel::kRetryableError);
        }
        return bob.obj();
    }

    Milliseconds advanceUntilReadyRequest() const {
        using namespace std::literals;
        auto net = getNetworkInterfaceMock();

        std::this_thread::sleep_for(1ms);
        auto totalWaited = Milliseconds{0};
        auto _ = executor::NetworkInterfaceMock::InNetworkGuard{net};
        while (!net->hasReadyRequests()) {
            auto advance = Milliseconds{10};
            net->advanceTime(net->now() + advance);
            totalWaited += advance;
            std::this_thread::sleep_for(100us);
        }
        return totalWaited;
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

    void scheduleRequestAndAdvanceClockForRetry(std::shared_ptr<mongo::RetryStrategy> retryStrategy,
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
    SemiFuture<HostAndPort> resolve(CancellationToken t, const TargetingMetadata&) final {
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

    SemiFuture<HostAndPort> resolve(CancellationToken t,
                                    const TargetingMetadata& targetingMetadata) final {
        const auto notDeprioritized = [&](const HostAndPort& server) {
            return std::ranges::find(targetingMetadata.deprioritizedServers, server) ==
                targetingMetadata.deprioritizedServers.end();
        };

        if (auto it = std::ranges::find_if(_resolvedHosts, notDeprioritized);
            it != _resolvedHosts.end()) {
            return SemiFuture<HostAndPort>::makeReady(*it);
        }

        return SemiFuture<HostAndPort>::makeReady(_resolvedHosts.front());
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
