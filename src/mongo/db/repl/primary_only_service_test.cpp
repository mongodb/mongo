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

#include <memory>

#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"

using namespace mongo;
using namespace mongo::repl;

constexpr StringData kTestServiceName = "TestService"_sd;

class TestService final : public PrimaryOnlyService {
public:
    explicit TestService(ServiceContext* serviceContext) : PrimaryOnlyService(serviceContext) {}
    ~TestService() = default;

    StringData getServiceName() const override {
        return kTestServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString("admin", "test_service");
    }

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(
        const BSONObj& initialState) const override {
        return std::make_shared<TestService::Instance>(initialState);
    }

    class Instance final : public PrimaryOnlyService::TypedInstance<Instance> {
    public:
        Instance(const BSONObj& state)
            : PrimaryOnlyService::TypedInstance<Instance>(), _state(state) {}

        void run(std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept override {
            _completionPromise.emplaceValue();
        }

        void waitForCompletion() {
            _completionPromise.getFuture().wait();
        }

        const BSONObj& getState() const {
            return _state;
        }

    private:
        BSONObj _state;
        SharedPromise<void> _completionPromise;
    };
};

class PrimaryOnlyServiceTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto serviceContext = getServiceContext();

        WaitForMajorityService::get(getServiceContext()).setUp(getServiceContext());

        auto opCtx = cc().makeOperationContext();
        {
            auto replCoord = std::make_unique<ReplicationCoordinatorMock>(serviceContext);
            ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_PRIMARY));
            ASSERT_OK(replCoord->updateTerm(opCtx.get(), 1));
            ReplicationCoordinator::set(serviceContext, std::move(replCoord));

            repl::setOplogCollectionName(serviceContext);
            repl::createOplog(opCtx.get());
            // Set up OpObserver so that repl::logOp() will store the oplog entry's optime in
            // ReplClientInfo.
            OpObserverRegistry* opObserverRegistry =
                dynamic_cast<OpObserverRegistry*>(serviceContext->getOpObserver());
            opObserverRegistry->addObserver(std::make_unique<OpObserverImpl>());
        }

        _registry = std::make_unique<PrimaryOnlyServiceRegistry>();
        {
            std::unique_ptr<TestService> service =
                std::make_unique<TestService>(getServiceContext());

            _registry->registerService(std::move(service));
            _registry->onStartup(opCtx.get());
            _registry->onStepUpComplete(nullptr, 1);
        }

        _service = _registry->lookupService("TestService");
        ASSERT(_service);
    }

    void tearDown() override {
        _registry->shutdown();
        _registry.reset();
        _service = nullptr;

        WaitForMajorityService::get(getServiceContext()).shutDown();

        ServiceContextMongoDTest::tearDown();
    }

protected:
    std::unique_ptr<PrimaryOnlyServiceRegistry> _registry;
    PrimaryOnlyService* _service;
};

DEATH_TEST_F(PrimaryOnlyServiceTest,
             DoubleRegisterService,
             "Attempted to register PrimaryOnlyService (TestService) that is already registered") {
    PrimaryOnlyServiceRegistry registry;

    std::unique_ptr<TestService> service1 = std::make_unique<TestService>(getServiceContext());
    std::unique_ptr<TestService> service2 = std::make_unique<TestService>(getServiceContext());

    registry.registerService(std::move(service1));
    registry.registerService(std::move(service2));
}

TEST_F(PrimaryOnlyServiceTest, BasicCreateInstance) {
    auto instance = TestService::Instance::getOrCreate(_service,
                                                       BSON("_id" << 0 << "state"
                                                                  << "foo"));
    ASSERT(instance.get());
    ASSERT_EQ(0, instance->getState()["_id"].Int());
    ASSERT_EQ("foo", instance->getState()["state"].String());
    instance->waitForCompletion();
}

TEST_F(PrimaryOnlyServiceTest, LookupInstance) {
    auto instance = TestService::Instance::getOrCreate(_service,
                                                       BSON("_id" << 0 << "state"
                                                                  << "foo"));
    ASSERT(instance.get());
    ASSERT_EQ(0, instance->getState()["_id"].Int());
    ASSERT_EQ("foo", instance->getState()["state"].String());

    auto instance2 = TestService::Instance::lookup(_service, BSON("_id" << 0));

    ASSERT(instance2.get());
    ASSERT_EQ(instance.get(), instance2.get().get());
    ASSERT_EQ(0, instance2.get()->getState()["_id"].Int());
    ASSERT_EQ("foo", instance2.get()->getState()["state"].String());
}

TEST_F(PrimaryOnlyServiceTest, DoubleCreateInstance) {
    auto instance = TestService::Instance::getOrCreate(_service,
                                                       BSON("_id" << 0 << "state"
                                                                  << "foo"));
    ASSERT(instance.get());
    ASSERT_EQ(0, instance->getState()["_id"].Int());
    ASSERT_EQ("foo", instance->getState()["state"].String());

    // Trying to create a new instance with the same _id but different state otherwise just returns
    // the already existing instance based on the _id only.
    auto instance2 = TestService::Instance::getOrCreate(_service,
                                                        BSON("_id" << 0 << "state"
                                                                   << "bar"));
    ASSERT_EQ(instance.get(), instance2.get());
    ASSERT_EQ(0, instance2->getState()["_id"].Int());
    ASSERT_EQ("foo", instance2->getState()["state"].String());
}

TEST_F(PrimaryOnlyServiceTest, CreateWhenNotPrimary) {
    _registry->onStepDown();

    ASSERT_THROWS_CODE(TestService::Instance::getOrCreate(_service,
                                                          BSON("_id" << 0 << "state"
                                                                     << "foo")),
                       DBException,
                       ErrorCodes::NotMaster);
}
