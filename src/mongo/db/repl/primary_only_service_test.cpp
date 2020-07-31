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
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"

using namespace mongo;
using namespace mongo::repl;

namespace {
constexpr StringData kTestServiceName = "TestService"_sd;

MONGO_FAIL_POINT_DEFINE(TestServiceHangDuringInitialization);
MONGO_FAIL_POINT_DEFINE(TestServiceHangDuringStateOne);
MONGO_FAIL_POINT_DEFINE(TestServiceHangDuringStateTwo);
MONGO_FAIL_POINT_DEFINE(TestServiceHangDuringCompletion);
}  // namespace

class TestService final : public PrimaryOnlyService {
public:
    enum class State {
        kInitializing = 0,
        kOne = 1,
        kTwo = 2,
        kDone = 3,
    };

    explicit TestService(ServiceContext* serviceContext) : PrimaryOnlyService(serviceContext) {}
    ~TestService() = default;

    StringData getServiceName() const override {
        return kTestServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString("admin", "test_service");
    }

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) const override {
        return std::make_shared<TestService::Instance>(std::move(initialState));
    }

    class Instance final : public PrimaryOnlyService::TypedInstance<Instance> {
    public:
        explicit Instance(BSONObj stateDoc)
            : PrimaryOnlyService::TypedInstance<Instance>(),
              _id(stateDoc["_id"].wrap().getOwned()),
              _stateDoc(std::move(stateDoc)),
              _state((State)_stateDoc["state"].Int()),
              _initialState(_state) {}

        void run(std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept override {
            if (MONGO_unlikely(TestServiceHangDuringInitialization.shouldFail())) {
                TestServiceHangDuringInitialization.pauseWhileSet();
            }

            SemiFuture<void>::makeReady()
                .thenRunOn(**executor)
                .then([this] {
                    _runOnce(State::kInitializing, State::kOne);

                    if (MONGO_unlikely(TestServiceHangDuringStateOne.shouldFail())) {
                        TestServiceHangDuringStateOne.pauseWhileSet();
                    }
                })
                .then([this] {
                    _runOnce(State::kOne, State::kTwo);

                    if (MONGO_unlikely(TestServiceHangDuringStateTwo.shouldFail())) {
                        TestServiceHangDuringStateTwo.pauseWhileSet();
                    }
                })
                .then([this] {
                    _runOnce(State::kTwo, State::kDone);

                    if (MONGO_unlikely(TestServiceHangDuringCompletion.shouldFail())) {
                        TestServiceHangDuringCompletion.pauseWhileSet();
                    }
                })

                .getAsync([this](auto) { _completionPromise.emplaceValue(); });
        }

        void waitForCompletion() {
            _completionPromise.getFuture().wait();
        }

        int getID() {
            stdx::lock_guard lk(_mutex);
            return _id["_id"].Int();
        }

        State getState() {
            stdx::lock_guard lk(_mutex);
            return _state;
        }

        State getInitialState() {
            stdx::lock_guard lk(_mutex);
            return _initialState;
        }

    private:
        void _runOnce(State sourceState, State targetState) {
            stdx::unique_lock lk(_mutex);
            if (_state > sourceState) {
                invariant(_state != State::kDone);
                return;
            }
            invariant(_state == sourceState);

            BSONObj newStateDoc = ([&] {
                BSONObjBuilder newStateDoc;
                newStateDoc.appendElements(_id);
                newStateDoc.append("state", (int)targetState);
                return newStateDoc.done().getOwned();
            })();
            _stateDoc = newStateDoc;
            _state = targetState;

            lk.unlock();

            auto opCtx = cc().makeOperationContext();
            DBDirectClient client(opCtx.get());
            if (targetState == State::kDone) {
                client.remove("admin.test_service", _id);
            } else {
                client.update("admin.test_service", _id, newStateDoc, true /*upsert*/);
            }
        }

        const InstanceID _id;
        BSONObj _stateDoc;
        State _state = State::kInitializing;
        const State _initialState;
        SharedPromise<void> _completionPromise;
        Mutex _mutex = MONGO_MAKE_LATCH("PrimaryOnlyServiceTest::TestService::_mutex");
    };
};

// Make ASSERT_EQ work with State enums.
std::ostream& operator<<(std::ostream& os, const TestService::State& state) {
    os << static_cast<int>(state);
    return os;
}

class PrimaryOnlyServiceTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto serviceContext = getServiceContext();

        WaitForMajorityService::get(getServiceContext()).setUp(getServiceContext());

        {
            auto opCtx = cc().makeOperationContext();
            auto replCoord = std::make_unique<ReplicationCoordinatorMock>(serviceContext);
            ReplicationCoordinator::set(serviceContext, std::move(replCoord));

            repl::setOplogCollectionName(serviceContext);
            repl::createOplog(opCtx.get());
            // Set up OpObserver so that repl::logOp() will store the oplog entry's optime in
            // ReplClientInfo.
            OpObserverRegistry* opObserverRegistry =
                dynamic_cast<OpObserverRegistry*>(serviceContext->getOpObserver());
            opObserverRegistry->addObserver(std::make_unique<OpObserverImpl>());


            _registry = std::make_unique<PrimaryOnlyServiceRegistry>();
            std::unique_ptr<TestService> service =
                std::make_unique<TestService>(getServiceContext());
            _registry->registerService(std::move(service));
            _registry->onStartup(opCtx.get());
        }
        stepUp();

        _service = _registry->lookupService("TestService");
        ASSERT(_service);

        _testExecutor = makeTestExecutor();
    }

    void tearDown() override {
        WaitForMajorityService::get(getServiceContext()).shutDown();

        _testExecutor->shutdown();
        _testExecutor->join();
        _testExecutor.reset();

        _registry->shutdown();
        _registry.reset();
        _service = nullptr;

        ServiceContextMongoDTest::tearDown();
    }

    void stepDown() {
        ASSERT_OK(ReplicationCoordinator::get(getServiceContext())
                      ->setFollowerMode(MemberState::RS_SECONDARY));
        _registry->onStepDown();
    }

    void stepUp() {
        auto opCtx = cc().makeOperationContext();
        auto replCoord = ReplicationCoordinator::get(getServiceContext());

        // Advance term
        _term++;

        ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_PRIMARY));
        ASSERT_OK(replCoord->updateTerm(opCtx.get(), _term));
        replCoord->setMyLastAppliedOpTimeAndWallTime(
            OpTimeAndWallTime(OpTime(Timestamp(1, 1), _term), Date_t()));

        _registry->onStepUpComplete(opCtx.get(), _term);
    }

    std::shared_ptr<executor::TaskExecutor> makeTestExecutor() {
        ThreadPool::Options threadPoolOptions;
        threadPoolOptions.threadNamePrefix = "PrimaryOnlyServiceTest-";
        threadPoolOptions.poolName = "PrimaryOnlyServiceTestThreadPool";
        threadPoolOptions.onCreateThread = [](const std::string& threadName) {
            Client::initThread(threadName.c_str());
        };

        auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
        auto executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
            std::make_unique<ThreadPool>(threadPoolOptions),
            executor::makeNetworkInterface(
                "PrimaryOnlyServiceTestNetwork", nullptr, std::move(hookList)));
        executor->startup();
        return executor;
    }

protected:
    std::unique_ptr<PrimaryOnlyServiceRegistry> _registry;
    PrimaryOnlyService* _service;
    long long _term = 0;
    std::shared_ptr<executor::TaskExecutor> _testExecutor;
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
    auto instance = TestService::Instance::getOrCreate(_service, BSON("_id" << 0 << "state" << 0));
    ASSERT(instance.get());
    ASSERT_EQ(0, instance->getID());
    ASSERT_EQ(TestService::State::kInitializing, instance->getInitialState());

    auto instance2 = TestService::Instance::getOrCreate(_service, BSON("_id" << 1 << "state" << 0));
    ASSERT(instance2.get());
    ASSERT_EQ(1, instance2->getID());
    ASSERT_EQ(TestService::State::kInitializing, instance2->getInitialState());

    instance->waitForCompletion();
    ASSERT_EQ(TestService::State::kDone, instance->getState());

    instance2->waitForCompletion();
    ASSERT_EQ(TestService::State::kDone, instance2->getState());
}

TEST_F(PrimaryOnlyServiceTest, LookupInstance) {
    auto instance = TestService::Instance::getOrCreate(_service, BSON("_id" << 0 << "state" << 0));
    ASSERT(instance.get());
    ASSERT_EQ(0, instance->getID());

    auto instance2 = TestService::Instance::lookup(_service, BSON("_id" << 0));

    ASSERT(instance2.get());
    ASSERT_EQ(instance.get(), instance2.get().get());
}

TEST_F(PrimaryOnlyServiceTest, DoubleCreateInstance) {
    auto instance = TestService::Instance::getOrCreate(_service, BSON("_id" << 0 << "state" << 0));
    ASSERT(instance.get());
    ASSERT_EQ(0, instance->getID());

    // Trying to create a new instance with the same _id but different state otherwise just returns
    // the already existing instance based on the _id only.
    auto instance2 = TestService::Instance::getOrCreate(_service, BSON("_id" << 0 << "state" << 1));
    ASSERT_EQ(instance.get(), instance2.get());
}

TEST_F(PrimaryOnlyServiceTest, CreateWhenNotPrimary) {
    _registry->onStepDown();

    ASSERT_THROWS_CODE(
        TestService::Instance::getOrCreate(_service, BSON("_id" << 0 << "state" << 0)),
        DBException,
        ErrorCodes::NotMaster);
}

TEST_F(PrimaryOnlyServiceTest, CreateWithoutID) {
    ASSERT_THROWS_CODE(
        TestService::Instance::getOrCreate(_service, BSON("state" << 0)), DBException, 4908702);
}

TEST_F(PrimaryOnlyServiceTest, StepDownBeforePersisted) {
    // Prevent the instance from writing its initial state document to the storage engine.
    TestServiceHangDuringInitialization.setMode(FailPoint::alwaysOn);

    auto instance = TestService::Instance::getOrCreate(_service, BSON("_id" << 0 << "state" << 0));
    TestServiceHangDuringInitialization.waitForTimesEntered(1);
    stepDown();
    TestServiceHangDuringInitialization.setMode(FailPoint::off);

    stepUp();

    // Since the Instance never wrote its state document, it shouldn't be recreated on stepUp.
    auto recreatedInstance = TestService::Instance::lookup(_service, BSON("_id" << 0));
    ASSERT(!recreatedInstance.is_initialized());
}

TEST_F(PrimaryOnlyServiceTest, RecreateInstanceOnStepUp) {
    // Cause the Instance to be interrupted after writing its initial state document in state 1.
    TestServiceHangDuringStateOne.setMode(FailPoint::alwaysOn);

    auto instance = TestService::Instance::getOrCreate(_service, BSON("_id" << 0 << "state" << 0));
    TestServiceHangDuringStateOne.waitForTimesEntered(1);

    ASSERT_EQ(TestService::State::kInitializing, instance->getInitialState());
    ASSERT_EQ(TestService::State::kOne, instance->getState());

    stepDown();

    TestServiceHangDuringStateOne.setMode(FailPoint::off);
    TestServiceHangDuringStateTwo.setMode(FailPoint::alwaysOn);

    stepUp();

    auto recreatedInstance = TestService::Instance::lookup(_service, BSON("_id" << 0)).get();
    ASSERT_EQ(TestService::State::kOne, recreatedInstance->getInitialState());
    TestServiceHangDuringStateTwo.waitForTimesEntered(1);
    ASSERT_EQ(TestService::State::kTwo, recreatedInstance->getState());

    stepDown();

    TestServiceHangDuringStateTwo.setMode(FailPoint::off);
    TestServiceHangDuringCompletion.setMode(FailPoint::alwaysOn);

    stepUp();

    recreatedInstance = TestService::Instance::lookup(_service, BSON("_id" << 0)).get();
    ASSERT_EQ(TestService::State::kTwo, recreatedInstance->getInitialState());
    TestServiceHangDuringCompletion.waitForTimesEntered(1);
    ASSERT_EQ(TestService::State::kDone, recreatedInstance->getState());

    TestServiceHangDuringCompletion.setMode(FailPoint::off);
    recreatedInstance->waitForCompletion();

    // TODO(SERVER-50053): Assert that lookup() can no long find instance after it has completed.

    stepDown();
    stepUp();

    // No Instance should be recreated since the previous run completed successfully and deleted its
    // state document.
    auto nonExistentInstance = TestService::Instance::lookup(_service, BSON("_id" << 0));
    ASSERT(!nonExistentInstance.is_initialized());
}

TEST_F(PrimaryOnlyServiceTest, StepDownBeforeRebuildingInstances) {
    // Cause the Instance to be interrupted after writing its initial state document in state 1.
    auto stateOneFPTimesEntered = TestServiceHangDuringStateOne.setMode(FailPoint::alwaysOn);

    auto instance = TestService::Instance::getOrCreate(_service, BSON("_id" << 0 << "state" << 0));
    TestServiceHangDuringStateOne.waitForTimesEntered(++stateOneFPTimesEntered);

    ASSERT_EQ(TestService::State::kInitializing, instance->getInitialState());
    ASSERT_EQ(TestService::State::kOne, instance->getState());

    stepDown();

    // Let the running instance terminate.
    TestServiceHangDuringStateOne.setMode(FailPoint::off);
    // Make the instance rebuild on stepUp hang
    auto rebuildingFPTimesEntered =
        PrimaryOnlyServiceHangBeforeRebuildingInstances.setMode(FailPoint::alwaysOn);

    stepUp();

    // Start an async task to lookup the rebuilt instance, this will block waiting on the rebuild to
    // finish.
    auto getInstanceFuture = SemiFuture<void>::makeReady().thenRunOn(_testExecutor).then([this]() {
        return TestService::Instance::getOrCreate(_service, BSON("_id" << 0 << "state" << 0));
    });
    PrimaryOnlyServiceHangBeforeRebuildingInstances.waitForTimesEntered(++rebuildingFPTimesEntered);
    ASSERT_FALSE(getInstanceFuture.isReady());

    // Stepdown, interrupting the thread waiting for the rebuilt instance.
    stepDown();

    // Let the previous stepUp attempt continue and realize that the node has since stepped down.
    PrimaryOnlyServiceHangBeforeRebuildingInstances.setMode(FailPoint::off);
    ASSERT_THROWS_CODE(getInstanceFuture.get(), DBException, ErrorCodes::NotMaster);

    // Now do another stepUp that is allowed to complete this time.
    stateOneFPTimesEntered = TestServiceHangDuringStateOne.setMode(FailPoint::alwaysOn);
    stepUp();
    TestServiceHangDuringStateOne.waitForTimesEntered(++stateOneFPTimesEntered);

    instance = TestService::Instance::lookup(_service, BSON("_id" << 0)).get();
    ASSERT_EQ(TestService::State::kOne, instance->getInitialState());
    ASSERT_EQ(TestService::State::kOne, instance->getState());

    TestServiceHangDuringStateOne.setMode(FailPoint::off);

    instance->waitForCompletion();
}
