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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_time_metadata_hook.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"

using namespace mongo;
using namespace mongo::repl;

constexpr StringData kTestServiceName = "TestService"_sd;

class TestService final : public PrimaryOnlyService {
public:
    explicit TestService(ServiceContext* serviceContext)
        : PrimaryOnlyService(serviceContext), _executor(makeTaskExecutor(serviceContext)) {
        _executor->startup();
    }
    ~TestService() = default;

    std::shared_ptr<executor::TaskExecutor> makeTaskExecutor(ServiceContext* serviceContext) {
        ThreadPool::Options threadPoolOptions;
        threadPoolOptions.threadNamePrefix = getServiceName() + "-";
        threadPoolOptions.poolName = getServiceName() + "ThreadPool";
        threadPoolOptions.onCreateThread = [](const std::string& threadName) {
            Client::initThread(threadName.c_str());
            AuthorizationSession::get(cc())->grantInternalAuthorization(&cc());
        };

        auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
        hookList->addHook(std::make_unique<rpc::LogicalTimeMetadataHook>(serviceContext));
        return std::make_shared<executor::ThreadPoolTaskExecutor>(
            std::make_unique<ThreadPool>(threadPoolOptions),
            executor::makeNetworkInterface("TestServiceNetwork", nullptr, std::move(hookList)));
    }

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

    std::unique_ptr<executor::ScopedTaskExecutor> getTaskExecutor() override {
        return std::make_unique<executor::ScopedTaskExecutor>(_executor);
    }

    class Instance final : public PrimaryOnlyService::TypedInstance<Instance> {
    public:
        Instance(const BSONObj& state)
            : PrimaryOnlyService::TypedInstance<Instance>(), _state(state) {}

        SemiFuture<RunOnceResult> runOnce(OperationContext* opCtx) override {
            return SemiFuture<RunOnceResult>::makeReady(RunOnceResult::kComplete());
        }

        const BSONObj& getState() const {
            return _state;
        }

    private:
        BSONObj _state;
    };

private:
    std::shared_ptr<executor::TaskExecutor> _executor;
};

class PrimaryOnlyServiceTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto serviceContext = getServiceContext();

        WaitForMajorityService::get(getServiceContext()).setUp(getServiceContext());

        {
            auto opCtx = cc().makeOperationContext();

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

TEST_F(PrimaryOnlyServiceTest, BasicStartNewInstance) {
    BSONObj state = BSON("_id" << 0 << "state"
                               << "foo");

    // Check that state document collection is empty
    auto opCtx = makeOperationContext();
    DBDirectClient client(opCtx.get());
    auto obj = client.findOne(_service->getStateDocumentsNS().toString(), Query());
    ASSERT(obj.isEmpty());

    // Successfully start a new instance
    auto fut = _service->startNewInstance(opCtx.get(), state);
    auto instanceID = fut.get();
    ASSERT_EQ(0, instanceID["_id"].Int());

    // Check that the state document for the instance was created properly.
    obj = client.findOne(_service->getStateDocumentsNS().toString(), Query());
    ASSERT_EQ(0, obj["_id"].Int());
    ASSERT_EQ("foo", obj["state"].String());

    // Check that we can look up the Instance by ID.
    auto instance = TestService::Instance::lookup(_service, BSON("_id" << 0));
    ASSERT_EQ(0, instance.get()->getState()["_id"].Int());
    ASSERT_EQ("foo", instance.get()->getState()["state"].String());
}

TEST_F(PrimaryOnlyServiceTest, DoubleRegisterInstance) {
    BSONObj state1 = BSON("_id" << 0 << "state"
                                << "foo");
    BSONObj state2 = BSON("_id" << 0 << "state"
                                << "bar");

    auto opCtx = makeOperationContext();

    // Register instance with _id 0
    auto fut = _service->startNewInstance(opCtx.get(), std::move(state1));

    // Assert that registering a second instance with the same _id throws DuplicateKey
    ASSERT_THROWS_CODE(_service->startNewInstance(opCtx.get(), std::move(state2)),
                       DBException,
                       ErrorCodes::DuplicateKey);

    // Verify first instance was registered successfully.
    auto instanceID = fut.get();
    ASSERT_EQ(0, instanceID["_id"].Int());

    // Check that we can look up the Instance by ID.
    auto instance = TestService::Instance::lookup(_service, BSON("_id" << 0));
    ASSERT_EQ(0, instance.get()->getState()["_id"].Int());
    ASSERT_EQ("foo", instance.get()->getState()["state"].String());
}

TEST_F(PrimaryOnlyServiceTest, StepDownDuringRegisterInstance) {
    BSONObj state = BSON("_id" << 0 << "state"
                               << "foo");

    // Begin starting a new instance, but pause after writing the state document to disk.
    auto& fp = PrimaryOnlyServiceHangBeforeCreatingInstance;
    fp.setMode(FailPoint::alwaysOn);
    auto opCtx = makeOperationContext();
    auto fut = _service->startNewInstance(opCtx.get(), state);
    fp.waitForTimesEntered(1);

    // Now step down
    _registry->onStepDown();
    auto replCoord = ReplicationCoordinator::get(opCtx.get());
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    // Now allow startNewInstance to complete.
    fp.setMode(FailPoint::off);
    auto instanceID = fut.get();
    ASSERT_EQ(0, instanceID["_id"].Int());

    // Check that the Instance was not created since we were are no longer primary.
    auto instance = TestService::Instance::lookup(_service, BSON("_id" << 0));
    ASSERT_FALSE(instance.is_initialized());

    // Check that the state document for the instance was created properly.
    DBDirectClient client(opCtx.get());
    BSONObj obj = client.findOne(_service->getStateDocumentsNS().toString(), Query());
    ASSERT_EQ(0, obj["_id"].Int());
    ASSERT_EQ("foo", obj["state"].String());
}
