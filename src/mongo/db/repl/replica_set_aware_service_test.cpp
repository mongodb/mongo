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

#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/database_holder_mock.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state_factory_shard.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state_factory_mock.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
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

    void onShutdown() override {}
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
