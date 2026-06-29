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

#include "mongo/db/repl/replica_set_aware_service.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state_factory_shard.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/db/shard_role/shard_catalog/database_holder_mock.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state_factory_mock.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <ratio>

#include <boost/move/utility_core.hpp>

namespace mongo {

namespace {

template <class ActualService>
class TestService : public ReplicaSetAwareService<ActualService> {
public:
    int numCallsOnStartup{0};
    int numCallsOnShutdown{0};
    int numCallsOnSetCurrentConfig{0};
    int numCallsOnConsistentDataAvailable{0};
    int numCallsOnStepUpBegin{0};
    int numCallsOnStepUpComplete{0};
    int numCallsOnStepDown{0};
    int numCallsOnTransitionToRollback{0};
    int numCallsOnBecomeArbiter{0};

protected:
    void onStartup(OperationContext* opCtx) override {
        numCallsOnStartup++;
    }

    void onSetCurrentConfig(OperationContext* opCtx) override {
        numCallsOnSetCurrentConfig++;
    }

    void onConsistentDataAvailable(OperationContext* opCtx,
                                   bool isMajority,
                                   bool isRollback) override {
        numCallsOnConsistentDataAvailable++;
    }

    void onStepUpBegin(OperationContext* opCtx, long long term) override {
        numCallsOnStepUpBegin++;
    }

    void onStepUpComplete(OperationContext* opCtx, long long term) override {
        numCallsOnStepUpComplete++;
    }

    void onStepDown() override {
        numCallsOnStepDown++;
    }

    void onRollbackBegin() override {
        numCallsOnTransitionToRollback++;
    }

    void onBecomeArbiter() override {
        numCallsOnBecomeArbiter++;
    }

    void onShutdown() override {
        numCallsOnShutdown++;
    }
};

/**
 * Service that's never registered.
 */
class ServiceA : public TestService<ServiceA> {
public:
    static ServiceA* get(ServiceContext* serviceContext);

private:
    bool shouldRegisterReplicaSetAwareService() const final {
        return false;
    }

    std::string getServiceName() const final {
        return "ServiceA";
    }
};

const auto getServiceA = ServiceContext::declareDecoration<ServiceA>();

ReplicaSetAwareServiceRegistry::Registerer<ServiceA> serviceARegisterer("ServiceA");

ServiceA* ServiceA::get(ServiceContext* serviceContext) {
    return &getServiceA(serviceContext);
}


/**
 * Service that's always registered.
 */
class ServiceB : public TestService<ServiceB> {
public:
    static ServiceB* get(ServiceContext* serviceContext);

private:
    bool shouldRegisterReplicaSetAwareService() const final {
        return true;
    }

    std::string getServiceName() const final {
        return "ServiceB";
    }
};

const auto getServiceB = ServiceContext::declareDecoration<ServiceB>();

ReplicaSetAwareServiceRegistry::Registerer<ServiceB> serviceBRegisterer("ServiceB");

ServiceB* ServiceB::get(ServiceContext* serviceContext) {
    return &getServiceB(serviceContext);
}


/**
 * Service that's always registered, depends on ServiceB.
 */
class ServiceC : public TestService<ServiceC> {
public:
    static ServiceC* get(ServiceContext* serviceContext);

private:
    ServiceContext* getServiceContext();

    bool shouldRegisterReplicaSetAwareService() const final {
        return true;
    }

    std::string getServiceName() const final {
        return "ServiceC";
    }

    void onStartup(OperationContext* opCtx) final {
        ASSERT_EQ(numCallsOnStartup, ServiceB::get(getServiceContext())->numCallsOnStartup - 1);
        TestService::onStartup(opCtx);
    }

    void onSetCurrentConfig(OperationContext* opCtx) final {
        ASSERT_EQ(numCallsOnSetCurrentConfig,
                  ServiceB::get(getServiceContext())->numCallsOnSetCurrentConfig - 1);
        TestService::onSetCurrentConfig(opCtx);
    }

    void onConsistentDataAvailable(OperationContext* opCtx,
                                   bool isMajority,
                                   bool isRollback) final {
        ASSERT_EQ(numCallsOnConsistentDataAvailable,
                  ServiceB::get(getServiceContext())->numCallsOnConsistentDataAvailable - 1);
        TestService::onConsistentDataAvailable(opCtx, isMajority, false);
    }

    void onStepUpBegin(OperationContext* opCtx, long long term) final {
        ASSERT_EQ(numCallsOnStepUpBegin,
                  ServiceB::get(getServiceContext())->numCallsOnStepUpBegin - 1);
        TestService::onStepUpBegin(opCtx, term);
    }

    void onStepUpComplete(OperationContext* opCtx, long long term) final {
        ASSERT_EQ(numCallsOnStepUpComplete,
                  ServiceB::get(getServiceContext())->numCallsOnStepUpComplete - 1);
        TestService::onStepUpComplete(opCtx, term);
    }

    void onStepDown() final {
        ASSERT_EQ(numCallsOnStepDown, ServiceB::get(getServiceContext())->numCallsOnStepDown - 1);
        TestService::onStepDown();
    }

    void onRollbackBegin() final {
        ASSERT_EQ(numCallsOnTransitionToRollback,
                  ServiceB::get(getServiceContext())->numCallsOnTransitionToRollback - 1);
        TestService::onRollbackBegin();
    }

    void onBecomeArbiter() final {
        ASSERT_EQ(numCallsOnBecomeArbiter,
                  ServiceB::get(getServiceContext())->numCallsOnBecomeArbiter - 1);
        TestService::onBecomeArbiter();
    }
};

const auto getServiceC = ServiceContext::declareDecoration<ServiceC>();

ReplicaSetAwareServiceRegistry::Registerer<ServiceC> serviceCRegisterer("ServiceC", {"ServiceB"});

ServiceC* ServiceC::get(ServiceContext* serviceContext) {
    return &getServiceC(serviceContext);
}

ServiceContext* ServiceC::getServiceContext() {
    return getServiceC.owner(this);
}

/*
 * Service that can be configured to sleep for specified amount of time in its onStepUpBegin and
 * onStepUpComplete methods. Used for testing that we log when a service takes a long time.
 */
class SlowService : public TestService<SlowService> {
public:
    static SlowService* get(ServiceContext* serviceContext);

    void setStepUpBeginSleepDuration(Duration<std::milli> duration) {
        _stepUpBeginSleepDuration = duration;
    }

    void setStepUpCompleteSleepDuration(Duration<std::milli> duration) {
        _stepUpCompleteSleepDuration = duration;
    }

    void setStepDownSleepDuration(Duration<std::milli> duration) {
        _stepDownSleepDuration = duration;
    }

private:
    Duration<std::milli> _stepUpBeginSleepDuration = Milliseconds(0);
    Duration<std::milli> _stepUpCompleteSleepDuration = Milliseconds(0);
    Duration<std::milli> _stepDownSleepDuration = Milliseconds(0);

    ServiceContext* getServiceContext();

    bool shouldRegisterReplicaSetAwareService() const final {
        return true;
    }

    std::string getServiceName() const final {
        return "SlowService";
    }

    void onStepUpBegin(OperationContext* opCtx, long long term) final {
        sleepFor(_stepUpBeginSleepDuration);
        TestService::onStepUpBegin(opCtx, term);
    }

    void onStepUpComplete(OperationContext* opCtx, long long term) final {
        sleepFor(_stepUpCompleteSleepDuration);
        TestService::onStepUpComplete(opCtx, term);
    }

    void onStepDown() final {
        sleepFor(_stepDownSleepDuration);
        TestService::onStepDown();
    }
};

const auto getSlowService = ServiceContext::declareDecoration<SlowService>();
ReplicaSetAwareServiceRegistry::Registerer<SlowService> slowServiceRegister("SlowService");

SlowService* SlowService::get(ServiceContext* serviceContext) {
    return &getSlowService(serviceContext);
}

ServiceContext* SlowService::getServiceContext() {
    return getSlowService.owner(this);
}

/**
 * Service whose onStartup can be made to block until the test explicitly unblocks it. Used to
 * exercise shutdown waiting for an in-progress startup to complete.
 */
class BlockingService : public TestService<BlockingService> {
public:
    static BlockingService* get(ServiceContext* serviceContext);

    void enableStartupBlocking() {
        _blockStartup.store(true);
    }

    void waitForStartupReached() {
        _startupReached.get();
    }

    void unblockStartup() {
        _proceedWithStartup.set();
    }

private:
    bool shouldRegisterReplicaSetAwareService() const final {
        return true;
    }

    std::string getServiceName() const final {
        return "BlockingService";
    }

    void onStartup(OperationContext* opCtx) final {
        if (_blockStartup.load()) {
            _startupReached.set();
            _proceedWithStartup.get();
        }
        TestService::onStartup(opCtx);
    }

    AtomicWord<bool> _blockStartup{false};
    Notification<void> _startupReached;
    Notification<void> _proceedWithStartup;
};

const auto getBlockingService = ServiceContext::declareDecoration<BlockingService>();
ReplicaSetAwareServiceRegistry::Registerer<BlockingService> blockingServiceRegisterer(
    "BlockingService");

BlockingService* BlockingService::get(ServiceContext* serviceContext) {
    return &getBlockingService(serviceContext);
}

/**
 * Service registered to run after BlockingService (it depends on it). Used to verify that an
 * in-progress startup stops early once a shutdown is requested: this service must not be started in
 * that case, even though it is still shut down.
 */
class DownstreamService : public TestService<DownstreamService> {
public:
    static DownstreamService* get(ServiceContext* serviceContext);

private:
    bool shouldRegisterReplicaSetAwareService() const final {
        return true;
    }

    std::string getServiceName() const final {
        return "DownstreamService";
    }
};

const auto getDownstreamService = ServiceContext::declareDecoration<DownstreamService>();
ReplicaSetAwareServiceRegistry::Registerer<DownstreamService> downstreamServiceRegisterer(
    "DownstreamService", {"BlockingService"});

DownstreamService* DownstreamService::get(ServiceContext* serviceContext) {
    return &getDownstreamService(serviceContext);
}

class ReplicaSetAwareServiceTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();

        auto serviceContext = getServiceContext();
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext);
        replCoord->setMyLastAppliedOpTimeAndWallTimeForward(
            repl::OpTimeAndWallTime(repl::OpTime(Timestamp(1, 1), _term), Date_t()));
        repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));

        DatabaseHolder::set(getServiceContext(), std::make_unique<DatabaseHolderMock>());

        DatabaseShardingStateFactory::set(getServiceContext(),
                                          std::make_unique<DatabaseShardingStateFactoryMock>());

        CollectionShardingStateFactory::set(
            getServiceContext(),
            std::make_unique<CollectionShardingStateFactoryShard>(getServiceContext()));
        ShardingState::create(getServiceContext());
    }

protected:
    ReplicaSetAwareServiceRegistry& registry() {
        return ReplicaSetAwareServiceRegistry::get(getGlobalServiceContext());
    }

    long long _term = 1;
    repl::ReplSetConfig _replSetConfig;

    // Skip recovering user writes critical sections because the fixture doesn't construct
    // ServiceEntryPoint and this causes a segmentation fault when
    // UserWritesRecoverableCriticalSectionService uses DBDirectClient to call into
    // ServiceEntryPoint
    FailPointEnableBlock skipRecoverUserWriteCriticalSections{
        "skipRecoverUserWriteCriticalSections"};
    // Disable the QueryAnalysisCoordinator for the same reason as the above.
    FailPointEnableBlock disableQueryAnalysisCoordinator{"disableQueryAnalysisCoordinator"};
    // Disable the QueryAnalysisWriter because the fixture doesn't construct the ServiceEntryPoint
    // or the PeriodicRunner.
    FailPointEnableBlock disableQueryAnalysisWriter{"disableQueryAnalysisWriter"};
    // Disable direct connection checks because this fixture doesn't set up the ShardingState
    FailPointEnableBlock _skipDirectConnectionChecks{"skipDirectConnectionChecks"};
};


TEST_F(ReplicaSetAwareServiceTest, ReplicaSetAwareService) {
    auto sc = getGlobalServiceContext();
    auto opCtxHolder = makeOperationContext();
    auto opCtx = opCtxHolder.get();

    auto a = ServiceA::get(sc);
    auto b = ServiceB::get(sc);
    auto c = ServiceC::get(sc);

    ASSERT_EQ(0, a->numCallsOnStartup);
    ASSERT_EQ(0, a->numCallsOnSetCurrentConfig);
    ASSERT_EQ(0, a->numCallsOnConsistentDataAvailable);
    ASSERT_EQ(0, a->numCallsOnStepUpBegin);
    ASSERT_EQ(0, a->numCallsOnStepUpComplete);
    ASSERT_EQ(0, a->numCallsOnStepDown);
    ASSERT_EQ(0, a->numCallsOnTransitionToRollback);
    ASSERT_EQ(0, a->numCallsOnBecomeArbiter);

    ASSERT_EQ(0, b->numCallsOnStartup);
    ASSERT_EQ(0, b->numCallsOnSetCurrentConfig);
    ASSERT_EQ(0, b->numCallsOnConsistentDataAvailable);
    ASSERT_EQ(0, b->numCallsOnStepUpBegin);
    ASSERT_EQ(0, b->numCallsOnStepUpComplete);
    ASSERT_EQ(0, b->numCallsOnStepDown);
    ASSERT_EQ(0, b->numCallsOnTransitionToRollback);
    ASSERT_EQ(0, b->numCallsOnBecomeArbiter);

    ASSERT_EQ(0, c->numCallsOnStartup);
    ASSERT_EQ(0, c->numCallsOnSetCurrentConfig);
    ASSERT_EQ(0, c->numCallsOnConsistentDataAvailable);
    ASSERT_EQ(0, c->numCallsOnStepUpBegin);
    ASSERT_EQ(0, c->numCallsOnStepUpComplete);
    ASSERT_EQ(0, c->numCallsOnStepDown);
    ASSERT_EQ(0, c->numCallsOnTransitionToRollback);
    ASSERT_EQ(0, c->numCallsOnBecomeArbiter);

    ReplicaSetAwareServiceRegistry::get(sc).onStartup(opCtx);
    ReplicaSetAwareServiceRegistry::get(sc).onSetCurrentConfig(opCtx);
    ReplicaSetAwareServiceRegistry::get(sc).onSetCurrentConfig(opCtx);
    ReplicaSetAwareServiceRegistry::get(sc).onConsistentDataAvailable(
        opCtx, false /* isMajority */, false /* isRollback */);
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpBegin(opCtx, _term);
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpBegin(opCtx, _term);
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpBegin(opCtx, _term);
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpComplete(opCtx, _term);
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpComplete(opCtx, _term);
    ReplicaSetAwareServiceRegistry::get(sc).onStepDown();
    ReplicaSetAwareServiceRegistry::get(sc).onRollbackBegin();
    ReplicaSetAwareServiceRegistry::get(sc).onBecomeArbiter();

    ASSERT_EQ(0, a->numCallsOnStartup);
    ASSERT_EQ(0, a->numCallsOnSetCurrentConfig);
    ASSERT_EQ(0, a->numCallsOnConsistentDataAvailable);
    ASSERT_EQ(0, a->numCallsOnStepUpBegin);
    ASSERT_EQ(0, a->numCallsOnStepUpComplete);
    ASSERT_EQ(0, a->numCallsOnStepDown);
    ASSERT_EQ(0, a->numCallsOnTransitionToRollback);
    ASSERT_EQ(0, a->numCallsOnBecomeArbiter);

    ASSERT_EQ(1, b->numCallsOnStartup);
    ASSERT_EQ(2, b->numCallsOnSetCurrentConfig);
    ASSERT_EQ(1, b->numCallsOnConsistentDataAvailable);
    ASSERT_EQ(3, b->numCallsOnStepUpBegin);
    ASSERT_EQ(2, b->numCallsOnStepUpComplete);
    ASSERT_EQ(1, b->numCallsOnStepDown);
    ASSERT_EQ(1, b->numCallsOnTransitionToRollback);
    ASSERT_EQ(1, b->numCallsOnBecomeArbiter);

    ASSERT_EQ(1, c->numCallsOnStartup);
    ASSERT_EQ(2, c->numCallsOnSetCurrentConfig);
    ASSERT_EQ(1, c->numCallsOnConsistentDataAvailable);
    ASSERT_EQ(3, c->numCallsOnStepUpBegin);
    ASSERT_EQ(2, c->numCallsOnStepUpComplete);
    ASSERT_EQ(1, c->numCallsOnStepDown);
    ASSERT_EQ(1, c->numCallsOnTransitionToRollback);
    ASSERT_EQ(1, c->numCallsOnBecomeArbiter);
}

TEST_F(ReplicaSetAwareServiceTest, ReplicaSetAwareServiceCanStartThenShutdown) {
    auto sc = getGlobalServiceContext();
    auto opCtxHolder = makeOperationContext();
    auto opCtx = opCtxHolder.get();
    auto& reg = registry();

    auto b = ServiceB::get(sc);
    auto c = ServiceC::get(sc);

    reg.onStartup(opCtx);
    ASSERT_EQ(1, b->numCallsOnStartup);
    ASSERT_EQ(1, c->numCallsOnStartup);
    ASSERT_EQ(0, b->numCallsOnShutdown);
    ASSERT_EQ(0, c->numCallsOnShutdown);

    reg.onShutdown();
    ASSERT_EQ(1, b->numCallsOnStartup);
    ASSERT_EQ(1, c->numCallsOnStartup);
    ASSERT_EQ(1, b->numCallsOnShutdown);
    ASSERT_EQ(1, c->numCallsOnShutdown);
}

TEST_F(ReplicaSetAwareServiceTest, ReplicaSetAwareServiceCanShutdownWithoutStartup) {
    auto sc = getGlobalServiceContext();
    auto opCtxHolder = makeOperationContext();
    auto opCtx = opCtxHolder.get();
    auto& reg = registry();

    auto b = ServiceB::get(sc);
    auto c = ServiceC::get(sc);

    // Shutting down a never-started instance is allowed and still drives onShutdown per service.
    reg.onShutdown();
    ASSERT_EQ(0, b->numCallsOnStartup);
    ASSERT_EQ(0, c->numCallsOnStartup);
    ASSERT_EQ(1, b->numCallsOnShutdown);
    ASSERT_EQ(1, c->numCallsOnShutdown);

    // Once shutdown has occurred, a later startup is a no-op.
    reg.onStartup(opCtx);
    ASSERT_EQ(0, b->numCallsOnStartup);
    ASSERT_EQ(0, c->numCallsOnStartup);
}

TEST_F(ReplicaSetAwareServiceTest, ReplicaSetAwareServiceCannotStartTwice) {
    auto sc = getGlobalServiceContext();
    auto opCtxHolder = makeOperationContext();
    auto opCtx = opCtxHolder.get();
    auto& reg = registry();

    auto b = ServiceB::get(sc);
    auto c = ServiceC::get(sc);

    reg.onStartup(opCtx);
    ASSERT_EQ(1, b->numCallsOnStartup);
    ASSERT_EQ(1, c->numCallsOnStartup);

    // A second startup must be skipped (and logged at INFO); services must not be started again.
    unittest::LogCaptureGuard logs;
    reg.onStartup(opCtx);
    logs.stop();

    ASSERT_EQ(1, b->numCallsOnStartup);
    ASSERT_EQ(1, c->numCallsOnStartup);
    ASSERT_EQ(1, logs.countBSONContainingSubset(BSON("id" << 12968800)));
    // 12968800: "Skipping ReplicaSetAwareServiceRegistry startup";
}

TEST_F(ReplicaSetAwareServiceTest, ReplicaSetAwareServiceCannotShutdownTwice) {
    auto sc = getGlobalServiceContext();
    auto opCtxHolder = makeOperationContext();
    auto opCtx = opCtxHolder.get();
    auto& reg = registry();

    auto b = ServiceB::get(sc);
    auto c = ServiceC::get(sc);

    reg.onStartup(opCtx);
    reg.onShutdown();
    ASSERT_EQ(1, b->numCallsOnShutdown);
    ASSERT_EQ(1, c->numCallsOnShutdown);

    // A second shutdown must not run onShutdown on the services again.
    reg.onShutdown();
    ASSERT_EQ(1, b->numCallsOnShutdown);
    ASSERT_EQ(1, c->numCallsOnShutdown);
}

TEST_F(ReplicaSetAwareServiceTest, ReplicaSetAwareServiceShutdownInterruptsStartup) {
    auto sc = getGlobalServiceContext();
    auto& reg = registry();

    auto blockingService = BlockingService::get(sc);
    auto downstreamService = DownstreamService::get(sc);
    blockingService->enableStartupBlocking();

    // Run startup on a separate thread. It will block inside BlockingService::onStartup, leaving
    // the registry in its "startup in progress" state. DownstreamService is ordered after
    // BlockingService and has not been started yet.
    stdx::thread startupThread([&] {
        ThreadClient tc(sc->getService());
        auto opCtx = tc.get()->makeOperationContext();
        reg.onStartup(opCtx.get());
    });

    AtomicWord<bool> shutdownReturned{false};
    stdx::thread shutdownThread;

    // Always release startup and join the threads, even if an assertion throws below.
    ON_BLOCK_EXIT([&] {
        if (startupThread.joinable()) {
            blockingService->unblockStartup();
            startupThread.join();
        }
        if (shutdownThread.joinable()) {
            shutdownThread.join();
        }
    });

    // Wait until startup is actually in progress before initiating shutdown.
    blockingService->waitForStartupReached();

    // Run shutdown on a separate thread. Because startup is in progress, it must block rather than
    // shutting any service down.
    shutdownThread = stdx::thread([&] {
        reg.onShutdown();
        shutdownReturned.store(true);
    });

    // While startup is blocked, shutdown cannot make progress. Sleep briefly so that a non-waiting
    // implementation would have a chance to (incorrectly) proceed before we check.
    sleepFor(Milliseconds(100));
    ASSERT_FALSE(shutdownReturned.load());
    ASSERT_EQ(0, blockingService->numCallsOnShutdown);

    // Let the blocked startup resume. It should observe the shutdown request, stop before starting
    // DownstreamService, and let shutdown run to completion.
    blockingService->unblockStartup();
    startupThread.join();
    shutdownThread.join();

    // BlockingService had already entered onStartup, so it is started; DownstreamService comes
    // after it and must be skipped because the shutdown request breaks the startup loop.
    ASSERT_EQ(1, blockingService->numCallsOnStartup);
    ASSERT_EQ(0, downstreamService->numCallsOnStartup);
    // Both services are shut down regardless of whether they were started.
    ASSERT_EQ(1, blockingService->numCallsOnShutdown);
    ASSERT_EQ(1, downstreamService->numCallsOnShutdown);
    ASSERT_TRUE(shutdownReturned.load());
}

TEST_F(ReplicaSetAwareServiceTest, ReplicaSetAwareServiceLogSlowServices) {
    std::string slowSingleServiceStepUpBeginMsg =
        "Duration spent in ReplicaSetAwareServiceRegistry::onStepUpBegin for service exceeded "
        "slowServiceOnStepUpBeginThresholdMS";
    std::string slowSingleServiceStepDownMsg =
        "Duration spent in ReplicaSetAwareServiceRegistry::onStepDown for service exceeded "
        "slowServiceOnStepDownThresholdMS";
    std::string slowSingleServiceStepUpCompleteMsg =
        "Duration spent in ReplicaSetAwareServiceRegistry::onStepUpComplete for service "
        "exceeded slowServiceOnStepUpCompleteThresholdMS";
    std::string slowTotalTimeStepUpBeginMsg =
        "Duration spent in ReplicaSetAwareServiceRegistry::onStepUpBegin for all services "
        "exceeded slowTotalOnStepUpBeginThresholdMS";
    std::string slowTotalTimeStepUpCompleteMsg =
        "Duration spent in ReplicaSetAwareServiceRegistry::onStepUpComplete for all services "
        "exceeded slowTotalOnStepUpCompleteThresholdMS";

    auto sc = getGlobalServiceContext();
    auto opCtxHolder = makeOperationContext();
    auto opCtx = opCtxHolder.get();

    auto slowService = SlowService::get(sc);
    ASSERT_EQ(0, slowService->numCallsOnStepUpBegin);
    ASSERT_EQ(0, slowService->numCallsOnStepUpComplete);

    // With the default sleep interval (no sleep) we don't log anything.
    unittest::LogCaptureGuard logs;
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpBegin(opCtx, _term);
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpComplete(opCtx, _term);
    logs.stop();
    ASSERT_EQ(1, slowService->numCallsOnStepUpBegin);
    ASSERT_EQ(1, slowService->numCallsOnStepUpComplete);
    ASSERT_EQ(0,
              logs.countTextContaining(
                  "Duration spent in ReplicaSetAwareServiceRegistry::onStepUpBegin"));
    ASSERT_EQ(0,
              logs.countTextContaining(
                  "Duration spent in ReplicaSetAwareServiceRegistry::onStepUpComplete"));

    // Introduce delays at the minimum thresholds at which we will log for a single service.
    slowService->setStepUpBeginSleepDuration(
        Milliseconds(repl::slowServiceOnStepUpBeginThresholdMS.load() + 1));
    slowService->setStepUpCompleteSleepDuration(
        Milliseconds(repl::slowServiceOnStepUpCompleteThresholdMS.load() + 1));
    slowService->setStepDownSleepDuration(
        Milliseconds(repl::slowServiceOnStepDownThresholdMS.load() + 1));
    logs.start();
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpBegin(opCtx, _term);
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpComplete(opCtx, _term);
    ReplicaSetAwareServiceRegistry::get(sc).onStepDown();
    logs.stop();
    ASSERT_EQ(2, slowService->numCallsOnStepUpBegin);
    ASSERT_EQ(2, slowService->numCallsOnStepUpComplete);
    ASSERT_EQ(1, logs.countTextContaining(slowSingleServiceStepUpBeginMsg));
    ASSERT_EQ(1, logs.countTextContaining(slowSingleServiceStepUpCompleteMsg));
    ASSERT_EQ(1, logs.countTextContaining(slowSingleServiceStepDownMsg));

    // Introduce a delay that should cause us to log for the total time across all services.
    slowService->setStepUpBeginSleepDuration(
        Milliseconds(repl::slowTotalOnStepUpBeginThresholdMS.load() + 1));
    slowService->setStepUpCompleteSleepDuration(
        Milliseconds(repl::slowTotalOnStepUpCompleteThresholdMS.load() + 1));
    logs.start();
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpBegin(opCtx, _term);
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpComplete(opCtx, _term);
    logs.stop();
    ASSERT_EQ(3, slowService->numCallsOnStepUpBegin);
    ASSERT_EQ(3, slowService->numCallsOnStepUpComplete);
    ASSERT_EQ(1, logs.countTextContaining(slowTotalTimeStepUpBeginMsg));
    ASSERT_EQ(1, logs.countTextContaining(slowTotalTimeStepUpCompleteMsg));
}

}  // namespace

}  // namespace mongo
