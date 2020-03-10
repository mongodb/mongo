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

#include "mongo/db/replica_set_aware_service.h"
#include "mongo/db/service_context_test_fixture.h"

namespace mongo {

namespace {

template <class ActualService>
class TestService : public ReplicaSetAwareService<ActualService> {
public:
    int numCallsOnStepUpBegin{0};
    int numCallsOnStepUpComplete{0};
    int numCallsOnStepDown{0};

protected:
    virtual void onStepUpBegin(OperationContext* opCtx) {
        numCallsOnStepUpBegin++;
    }

    virtual void onStepUpComplete(OperationContext* opCtx) {
        numCallsOnStepUpComplete++;
    }

    virtual void onStepDown() {
        numCallsOnStepDown++;
    }
};

/**
 * Service that's never registered.
 */
class ServiceA : public TestService<ServiceA> {
private:
    virtual bool shouldRegisterReplicaSetAwareService() const final {
        return false;
    }
};

ReplicaSetAwareServiceRegistry::Registerer<ServiceA> serviceARegisterer("ServiceA");


/**
 * Service that's always registered.
 */
class ServiceB : public TestService<ServiceB> {
private:
    virtual bool shouldRegisterReplicaSetAwareService() const final {
        return true;
    }
};

ReplicaSetAwareServiceRegistry::Registerer<ServiceB> serviceBRegisterer("ServiceB");


/**
 * Service that's always registered, depends on ServiceB.
 */
class ServiceC : public TestService<ServiceC> {
private:
    virtual bool shouldRegisterReplicaSetAwareService() const final {
        return true;
    }

    void onStepUpBegin(OperationContext* opCtx) final {
        ASSERT_EQ(numCallsOnStepUpBegin,
                  ServiceB::get(getServiceContext())->numCallsOnStepUpBegin - 1);
        TestService::onStepUpBegin(opCtx);
    }

    void onStepUpComplete(OperationContext* opCtx) final {
        ASSERT_EQ(numCallsOnStepUpComplete,
                  ServiceB::get(getServiceContext())->numCallsOnStepUpComplete - 1);
        TestService::onStepUpComplete(opCtx);
    }

    void onStepDown() final {
        ASSERT_EQ(numCallsOnStepDown, ServiceB::get(getServiceContext())->numCallsOnStepDown - 1);
        TestService::onStepDown();
    }
};

ReplicaSetAwareServiceRegistry::Registerer<ServiceC> serviceCRegisterer("ServiceC", {"ServiceB"});


using ReplicaSetAwareServiceTest = ServiceContextTest;


TEST_F(ReplicaSetAwareServiceTest, ReplicaSetAwareService) {
    auto sc = getGlobalServiceContext();
    auto opCtx = makeOperationContext().get();

    auto a = ServiceA::get(sc);
    auto b = ServiceB::get(sc);
    auto c = ServiceC::get(sc);

    ASSERT_EQ(0, a->numCallsOnStepUpBegin);
    ASSERT_EQ(0, a->numCallsOnStepUpComplete);
    ASSERT_EQ(0, a->numCallsOnStepDown);
    ASSERT_EQ(0, b->numCallsOnStepUpBegin);
    ASSERT_EQ(0, b->numCallsOnStepUpComplete);
    ASSERT_EQ(0, b->numCallsOnStepDown);
    ASSERT_EQ(0, c->numCallsOnStepUpBegin);
    ASSERT_EQ(0, c->numCallsOnStepUpComplete);
    ASSERT_EQ(0, c->numCallsOnStepDown);

    ReplicaSetAwareServiceRegistry::get(sc).onStepUpBegin(opCtx);
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpBegin(opCtx);
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpBegin(opCtx);
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpComplete(opCtx);
    ReplicaSetAwareServiceRegistry::get(sc).onStepUpComplete(opCtx);
    ReplicaSetAwareServiceRegistry::get(sc).onStepDown();

    ASSERT_EQ(0, a->numCallsOnStepUpBegin);
    ASSERT_EQ(0, a->numCallsOnStepUpComplete);
    ASSERT_EQ(0, a->numCallsOnStepDown);
    ASSERT_EQ(3, b->numCallsOnStepUpBegin);
    ASSERT_EQ(2, b->numCallsOnStepUpComplete);
    ASSERT_EQ(1, b->numCallsOnStepDown);
    ASSERT_EQ(3, c->numCallsOnStepUpBegin);
    ASSERT_EQ(2, c->numCallsOnStepUpComplete);
    ASSERT_EQ(1, c->numCallsOnStepDown);
}

}  // namespace

}  // namespace mongo
