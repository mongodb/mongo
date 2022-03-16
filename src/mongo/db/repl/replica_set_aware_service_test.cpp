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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_test_fixture.h"

namespace mongo {

namespace {

template <class ActualService>
class TestService : public ReplicaSetAwareService<ActualService> {
public:
    int numCallsOnStartup{0};
    int numCallsOnStartupRecoveryComplete{0};
    int numCallsOnInitialSyncComplete{0};
    int numCallsOnStepUpBegin{0};
    int numCallsOnStepUpComplete{0};
    int numCallsOnStepDown{0};
    int numCallsOnBecomeArbiter{0};

protected:
    void onStartup(OperationContext* opCtx) override {
        numCallsOnStartup++;
    }

    void onStartupRecoveryComplete(OperationContext* opCtx) override {
        numCallsOnStartupRecoveryComplete++;
    }

    void onInitialSyncComplete(OperationContext* opCtx) override {
        numCallsOnInitialSyncComplete++;
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

    void onStartup(OperationContext* opCtx) final {
        ASSERT_EQ(numCallsOnStartup, ServiceB::get(getServiceContext())->numCallsOnStartup - 1);
        TestService::onStartup(opCtx);
    }

    void onStartupRecoveryComplete(OperationContext* opCtx) final {
        ASSERT_EQ(numCallsOnStartupRecoveryComplete,
                  ServiceB::get(getServiceContext())->numCallsOnStartupRecoveryComplete - 1);
        TestService::onStartupRecoveryComplete(opCtx);
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


class ReplicaSetAwareServiceTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();

        auto serviceContext = getServiceContext();
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext);
        replCoord->setMyLastAppliedOpTimeAndWallTime(
            repl::OpTimeAndWallTime(repl::OpTime(Timestamp(1, 1), _term), Date_t()));
        repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));
    }

protected:
    long long _term = 1;
};


TEST_F(ReplicaSetAwareServiceTest, ReplicaSetAwareService) {
    auto sc = getGlobalServiceContext();
    auto opCtxHolder = makeOperationContext();
    auto opCtx = opCtxHolder.get();

    auto a = ServiceA::get(sc);
    auto b = ServiceB::get(sc);
    auto c = ServiceC::get(sc);

    ASSERT_EQ(0, a->numCallsOnStartup);
    ASSERT_EQ(0, a->numCallsOnStartupRecoveryComplete);
    ASSERT_EQ(0, a->numCallsOnInitialSyncComplete);
    ASSERT_EQ(0, a->numCallsOnStepUpBegin);
    ASSERT_EQ(0, a->numCallsOnStepUpComplete);
    ASSERT_EQ(0, a->numCallsOnStepDown);
    ASSERT_EQ(0, a->numCallsOnBecomeArbiter);

    ASSERT_EQ(0, b->numCallsOnStartup);
    ASSERT_EQ(0, b->numCallsOnStartupRecoveryComplete);
    ASSERT_EQ(0, b->numCallsOnInitialSyncComplete);
    ASSERT_EQ(0, b->numCallsOnStepUpBegin);
    ASSERT_EQ(0, b->numCallsOnStepUpComplete);
    ASSERT_EQ(0, b->numCallsOnStepDown);
    ASSERT_EQ(0, b->numCallsOnBecomeArbiter);

    ASSERT_EQ(0, c->numCallsOnStartup);
    ASSERT_EQ(0, c->numCallsOnStartupRecoveryComplete);
    ASSERT_EQ(0, c->numCallsOnInitialSyncComplete);
    ASSERT_EQ(0, c->numCallsOnStepUpBegin);
    ASSERT_EQ(0, c->numCallsOnStepUpComplete);
    ASSERT_EQ(0, c->numCallsOnStepDown);
    ASSERT_EQ(0, c->numCallsOnBecomeArbiter);

    ReplicaSetAwareServiceRegistry::get(sc).onStartup(opCtx);
    ReplicaSetAwareServiceRegistry::get(sc).onStartupRecoveryComplete(opCtx);
    ReplicaSetAwareServiceRegistry::get(sc).onInitialSyncComplete(opCtx);
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpBegin(opCtx, _term);
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpBegin(opCtx, _term);
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpBegin(opCtx, _term);
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpComplete(opCtx, _term);
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpComplete(opCtx, _term);
    ReplicaSetAwareServiceRegistry::get(sc).onStepDown();
    ReplicaSetAwareServiceRegistry::get(sc).onBecomeArbiter();

    ASSERT_EQ(0, a->numCallsOnStartup);
    ASSERT_EQ(0, a->numCallsOnStartupRecoveryComplete);
    ASSERT_EQ(0, a->numCallsOnInitialSyncComplete);
    ASSERT_EQ(0, a->numCallsOnStepUpBegin);
    ASSERT_EQ(0, a->numCallsOnStepUpComplete);
    ASSERT_EQ(0, a->numCallsOnStepDown);
    ASSERT_EQ(0, a->numCallsOnBecomeArbiter);

    ASSERT_EQ(1, b->numCallsOnStartup);
    ASSERT_EQ(1, b->numCallsOnStartupRecoveryComplete);
    ASSERT_EQ(1, b->numCallsOnInitialSyncComplete);
    ASSERT_EQ(3, b->numCallsOnStepUpBegin);
    ASSERT_EQ(2, b->numCallsOnStepUpComplete);
    ASSERT_EQ(1, b->numCallsOnStepDown);
    ASSERT_EQ(1, b->numCallsOnBecomeArbiter);

    ASSERT_EQ(1, c->numCallsOnStartup);
    ASSERT_EQ(1, c->numCallsOnStartupRecoveryComplete);
    ASSERT_EQ(1, c->numCallsOnInitialSyncComplete);
    ASSERT_EQ(3, c->numCallsOnStepUpBegin);
    ASSERT_EQ(2, c->numCallsOnStepUpComplete);
    ASSERT_EQ(1, c->numCallsOnStepDown);
    ASSERT_EQ(1, c->numCallsOnBecomeArbiter);
}

}  // namespace

}  // namespace mongo
