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

#include "mongo/db/repl/primary_only_service.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <memory>
#include <mutex>
#include <ostream>
#include <tuple>
#include <type_traits>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

using namespace mongo;
using namespace mongo::repl;

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace {
constexpr StringData kTestServiceName = "TestService"_sd;

MONGO_FAIL_POINT_DEFINE(TestServiceHangDuringInitialization);
MONGO_FAIL_POINT_DEFINE(TestServiceHangDuringStateOne);
MONGO_FAIL_POINT_DEFINE(TestServiceHangDuringStateTwo);
MONGO_FAIL_POINT_DEFINE(TestServiceHangDuringCompletion);
MONGO_FAIL_POINT_DEFINE(TestServiceHangBeforeMakingOpCtx);
MONGO_FAIL_POINT_DEFINE(TestServiceHangAfterMakingOpCtx);
MONGO_FAIL_POINT_DEFINE(TestServiceFailRebuildService);
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
    ~TestService() override = default;

    StringData getServiceName() const override {
        return kTestServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::createNamespaceString_forTest("config", "test_service");
    }

    ThreadPool::Limits getThreadPoolLimits() const override {
        return ThreadPool::Limits();
    }

    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) override {}

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialState) override {
        return std::make_shared<TestService::Instance>(this, std::move(initialState));
    }

    // Make this public since it's protected in the base class but it needs to be public if we want
    // to test it here.
    using PrimaryOnlyService::getAllInstances;

    class Instance final : public PrimaryOnlyService::TypedInstance<Instance> {
    public:
        explicit Instance(const TestService* service, BSONObj stateDoc)
            : PrimaryOnlyService::TypedInstance<Instance>(),
              _id(stateDoc["_id"].wrap().getOwned()),
              _stateDoc(std::move(stateDoc)),
              _state((State)_stateDoc["state"].Int()),
              _initialState(_state),
              _service(service) {}

        SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                             const CancellationToken& token) noexcept override {
            if (MONGO_unlikely(TestServiceHangDuringInitialization.shouldFail())) {
                TestServiceHangDuringInitialization.pauseWhileSet();
            }

            auto cancelLogicFinishedRunning =
                token.onCancel()
                    .thenRunOn(_service->getInstanceCleanupExecutor())
                    .then([this, self = shared_from_this()] {
                        stdx::lock_guard lk(_mutex);

                        if (_completionPromise.getFuture().isReady()) {
                            // We already completed
                            return;
                        }
                        _completionPromise.setError(Status(ErrorCodes::Interrupted, "Interrupted"));
                    });

            auto testLogicFinishedRunning =
                SemiFuture<void>::makeReady()
                    .thenRunOn(**executor)
                    .then([self = shared_from_this()] {
                        self->_runOnce(State::kInitializing, State::kOne);

                        if (MONGO_unlikely(TestServiceHangDuringStateOne.shouldFail())) {
                            TestServiceHangDuringStateOne.pauseWhileSet();
                        }
                    })
                    .then([self = shared_from_this()] {
                        self->_runOnce(State::kOne, State::kTwo);

                        if (MONGO_unlikely(TestServiceHangDuringStateTwo.shouldFail())) {
                            TestServiceHangDuringStateTwo.pauseWhileSet();
                        }
                    })
                    .then([self = shared_from_this()] {
                        // After this line the shared_ptr maintaining the Instance object is deleted
                        // from the PrimaryOnlyService's map.  Thus keeping the self reference is
                        // critical to extend the instance lifetime until all the callbacks using it
                        // have completed.
                        self->_runOnce(State::kTwo, State::kDone);

                        if (MONGO_unlikely(TestServiceHangDuringCompletion.shouldFail())) {
                            TestServiceHangDuringCompletion.pauseWhileSet();
                        }
                    })
                    .onCompletion([self = shared_from_this()](Status status) {
                        stdx::lock_guard lk(self->_mutex);
                        if (self->_completionPromise.getFuture().isReady()) {
                            // We were already interrupted
                            return;
                        }

                        if (status.isOK()) {
                            self->_completionPromise.emplaceValue();
                        } else {
                            self->_completionPromise.setError(status);
                        }
                    });

            // This instance is considered complete when both cancellation logic and test logic have
            // finished running.
            return whenAll(std::move(cancelLogicFinishedRunning),
                           std::move(testLogicFinishedRunning))
                .ignoreValue()
                .semi();
        }

        void checkIfOptionsConflict(const BSONObj&) const final {}

        void interrupt(Status status) override {
            // Currently unused. Functionality has been put into cancellation logic.
        }

        // Whether or not an op is reported depends on the "reportOp" field of the state doc the
        // Instance was created with.
        boost::optional<BSONObj> reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode connMode,
            MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override {
            stdx::lock_guard lk(_mutex);

            if (_stateDoc.getBoolField("reportOp")) {
                return BSON("instanceID" << _id << "state" << _state);
            } else {
                return boost::none;
            }
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

        SharedSemiFuture<void> getDocumentWriteException() {
            return _documentWriteException.getFuture();
        }

        SharedSemiFuture<void> getCompletionFuture() {
            return _completionPromise.getFuture();
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

            // Hang before creating OpCtx so that we can test that OpCtxs created after stepping
            // down still get interrupted.
            if (MONGO_unlikely(TestServiceHangBeforeMakingOpCtx.shouldFail())) {
                TestServiceHangBeforeMakingOpCtx.pauseWhileSet();
            }

            try {
                auto opCtx = cc().makeOperationContext();

                // Hang after creating OpCtx but before doing the document write so that we can test
                // that the OpCtx gets interrupted at stepdown or shutdown.
                if (MONGO_unlikely(TestServiceHangAfterMakingOpCtx.shouldFail())) {
                    TestServiceHangAfterMakingOpCtx.pauseWhileSet(opCtx.get());
                }

                DBDirectClient client(opCtx.get());
                if (targetState == State::kDone) {
                    client.remove(
                        NamespaceString::createNamespaceString_forTest("config.test_service"), _id);
                } else {
                    client.update(
                        NamespaceString::createNamespaceString_forTest("config.test_service"),
                        _id,
                        newStateDoc,
                        true /*upsert*/);
                }
            } catch (const DBException& e) {
                _documentWriteException.setError(e.toStatus());
                throw;
            }
        }

        const InstanceID _id;
        BSONObj _stateDoc;
        State _state = State::kInitializing;
        const State _initialState;
        SharedPromise<void> _completionPromise;
        // set only if doing a write to the state document throws an exception.
        SharedPromise<void> _documentWriteException;
        stdx::mutex _mutex;
        const TestService* const _service;
    };

private:
    ExecutorFuture<void> _rebuildService(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token) override {
        if (TestServiceFailRebuildService.shouldFail()) {
            uassertStatusOK(Status(ErrorCodes::InternalError, "_rebuildService error"));
        }
        auto nss = getStateDocumentsNS();

        AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
        auto opCtxHolder = cc().makeOperationContext();
        auto opCtx = opCtxHolder.get();
        DBDirectClient client(opCtx);
        BSONObj result;
        client.runCommand(nss.dbName(),
                          BSON("createIndexes"
                               << nss.coll() << "indexes"
                               << BSON_ARRAY(BSON("key" << BSON("x" << 1) << "name"
                                                        << "TestTTLIndex"
                                                        << "expireAfterSeconds" << 100000))),
                          result);
        uassertStatusOK(getStatusFromCommandResult(result));
        return ExecutorFuture<void>(**executor, Status::OK());
    };
};

// Make ASSERT_EQ work with State enums.
std::ostream& operator<<(std::ostream& os, const TestService::State& state) {
    os << static_cast<int>(state);
    return os;
}

class PrimaryOnlyServiceTest : public PrimaryOnlyServiceMongoDTest {
public:
    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<TestService>(serviceContext);
    }

    void setUp() override {
        PrimaryOnlyServiceMongoDTest::setUp();

        _service = _registry->lookupServiceByName("TestService");
        ASSERT(_service);
        auto serviceByNamespace = _registry->lookupServiceByNamespace(
            NamespaceString::createNamespaceString_forTest("config.test_service"));
        ASSERT_EQ(_service, serviceByNamespace);

        _testExecutor = makeTestExecutor();
    }

    void shutdownHook() override {
        _testExecutor->shutdown();
        _testExecutor->join();
        _testExecutor.reset();

        _registry->onShutdown();
        _service = nullptr;
    }

    void stepUp() {
        auto opCtx = cc().makeOperationContext();
        PrimaryOnlyServiceMongoDTest::stepUp(opCtx.get());
    }

    std::shared_ptr<executor::TaskExecutor> makeTestExecutor() {
        ThreadPool::Options threadPoolOptions;
        threadPoolOptions.threadNamePrefix = "PrimaryOnlyServiceTest-";
        threadPoolOptions.poolName = "PrimaryOnlyServiceTestThreadPool";
        threadPoolOptions.onCreateThread = [](const std::string& threadName) {
            Client::initThread(threadName, getGlobalServiceContext()->getService());
        };

        auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
        auto executor = executor::ThreadPoolTaskExecutor::create(
            std::make_unique<ThreadPool>(threadPoolOptions),
            executor::makeNetworkInterface(
                "PrimaryOnlyServiceTestNetwork", nullptr, std::move(hookList)));
        executor->startup();
        return executor;
    }

protected:
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

TEST_F(PrimaryOnlyServiceTest, CancellationOnStepdown) {
    // Used to ensure that _scheduleRun is run before we run the stepdown logic so that we fulfill
    // the _completionPromise.
    auto timesEntered = TestServiceHangDuringInitialization.setMode(FailPoint::alwaysOn);

    auto opCtx = makeOperationContext();
    auto instance =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
    ASSERT(instance.get());

    TestServiceHangDuringInitialization.waitForTimesEntered(timesEntered + 1);
    stepDown();
    TestServiceHangDuringInitialization.setMode(FailPoint::off);

    ASSERT_EQ(instance->getCompletionFuture().getNoThrow().code(), ErrorCodes::Interrupted);
}

TEST_F(PrimaryOnlyServiceTest, ResetCancellationSourceOnStepupAndCompleteSuccessfully) {
    {
        // Used to ensure that _scheduleRun is run before we run the stepdown logic so that we
        // fulfill the _completionPromise.
        auto timesEntered = TestServiceHangDuringInitialization.setMode(FailPoint::alwaysOn);

        auto opCtx = makeOperationContext();
        auto instance = TestService::Instance::getOrCreate(
            opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
        ASSERT(instance.get());

        TestServiceHangDuringInitialization.waitForTimesEntered(timesEntered + 1);
        stepDown();
        TestServiceHangDuringInitialization.setMode(FailPoint::off);

        ASSERT_EQ(instance->getCompletionFuture().getNoThrow().code(), ErrorCodes::Interrupted);
    }

    stepUp();

    {
        auto opCtx = makeOperationContext();
        auto instance = TestService::Instance::getOrCreate(
            opCtx.get(), _service, BSON("_id" << 1 << "state" << 0));
        ASSERT(instance.get());

        instance->getCompletionFuture().get();
    }
}

TEST_F(PrimaryOnlyServiceTest, ResetCancellationSourceOnStepupAndStepDownAgain) {
    {
        // Used to ensure that _scheduleRun is run before we run the stepdown logic so that we
        // fulfill the _completionPromise.
        auto timesEntered = TestServiceHangDuringInitialization.setMode(FailPoint::alwaysOn);

        auto opCtx = makeOperationContext();
        auto instance = TestService::Instance::getOrCreate(
            opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
        ASSERT(instance.get());

        TestServiceHangDuringInitialization.waitForTimesEntered(timesEntered + 1);
        stepDown();
        TestServiceHangDuringInitialization.setMode(FailPoint::off);

        ASSERT_EQ(instance->getCompletionFuture().getNoThrow().code(), ErrorCodes::Interrupted);
    }

    stepUp();

    {
        auto timesEntered = TestServiceHangDuringInitialization.setMode(FailPoint::alwaysOn);

        auto opCtx = makeOperationContext();
        auto instance = TestService::Instance::getOrCreate(
            opCtx.get(), _service, BSON("_id" << 1 << "state" << 0));
        ASSERT(instance.get());

        TestServiceHangDuringInitialization.waitForTimesEntered(timesEntered + 1);
        stepDown();
        TestServiceHangDuringInitialization.setMode(FailPoint::off);

        ASSERT_EQ(instance->getCompletionFuture().getNoThrow().code(), ErrorCodes::Interrupted);
    }
}

TEST_F(PrimaryOnlyServiceTest, StepUpAfterShutdown) {
    // TODO(SERVER-50612): Shutting down the WaitForMajorityService is a workaround for
    // SERVER-50612. After that is resolved this line should be able to be removed.
    WaitForMajorityService::get(getServiceContext()).shutDown();

    _registry->onShutdown();
    stepUp();
}

TEST_F(PrimaryOnlyServiceTest, ShutdownDuringStepUp) {
    stepDown();

    // Make the instance rebuild on stepUp hang
    auto stepUpTimesEntered =
        PrimaryOnlyServiceHangBeforeLaunchingStepUpLogic.setMode(FailPoint::alwaysOn);

    // Start an async task to step up, which will block on the fail point.
    auto stepUpFuture = ExecutorFuture<void>(_testExecutor).then([this]() { stepUp(); });
    PrimaryOnlyServiceHangBeforeLaunchingStepUpLogic.waitForTimesEntered(++stepUpTimesEntered);
    ASSERT_FALSE(stepUpFuture.isReady());

    // Shutdown, interrupting the thread waiting for the step up.
    shutdown();

    // Let the previous stepUp attempt continue and realize that the node has since shutdown.
    PrimaryOnlyServiceHangBeforeLaunchingStepUpLogic.setMode(FailPoint::off);
}

TEST_F(PrimaryOnlyServiceTest, BasicCreateInstance) {
    auto opCtx = makeOperationContext();
    auto instance =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
    ASSERT(instance.get());
    ASSERT_EQ(0, instance->getID());
    ASSERT_EQ(TestService::State::kInitializing, instance->getInitialState());

    auto instance2 =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 1 << "state" << 0));
    ASSERT(instance2.get());
    ASSERT_EQ(1, instance2->getID());
    ASSERT_EQ(TestService::State::kInitializing, instance2->getInitialState());

    instance->getCompletionFuture().get();
    ASSERT_EQ(TestService::State::kDone, instance->getState());

    instance2->getCompletionFuture().get();
    ASSERT_EQ(TestService::State::kDone, instance2->getState());
}

TEST_F(PrimaryOnlyServiceTest, LookupInstance) {
    // Make sure the instance doesn't complete before we try to look it up.
    TestServiceHangDuringInitialization.setMode(FailPoint::alwaysOn);

    auto opCtx = makeOperationContext();
    auto instance =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
    ASSERT(instance.get());
    ASSERT_EQ(0, instance->getID());

    auto [instance2, isPausedOrShutdown2] =
        TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0));
    ASSERT_TRUE(instance2);
    ASSERT_FALSE(isPausedOrShutdown2);

    ASSERT_EQ(instance.get(), instance2.value().get());

    TestServiceHangDuringInitialization.setMode(FailPoint::off);
    instance->getCompletionFuture().get();

    // Shouldn't be able to look up instance after it has completed running.
    auto [instance3, isPausedOrShutdown3] =
        TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0));
    ASSERT_FALSE(instance3);
    ASSERT_FALSE(isPausedOrShutdown3);
}

TEST_F(PrimaryOnlyServiceTest, LookupInstanceInterruptible) {
    stepDown();
    // Make sure the service stays in state kRebuilding so that lookup() has to try to wait on the
    // opCtx for the state to change.
    PrimaryOnlyServiceHangBeforeRebuildingInstances.setMode(FailPoint::alwaysOn);
    stepUp();

    auto opCtx = makeOperationContext();
    opCtx->markKilled(ErrorCodes::Interrupted);
    ASSERT_THROWS_CODE(TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0)),
                       DBException,
                       ErrorCodes::Interrupted);
}

TEST_F(PrimaryOnlyServiceTest, LookupInstanceHoldingISLock) {
    // Make sure the instance doesn't complete before we try to look it up.
    TestServiceHangDuringInitialization.setMode(FailPoint::alwaysOn);

    auto opCtx = makeOperationContext();
    auto instance =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
    ASSERT(instance.get());
    ASSERT_EQ(0, instance->getID());

    {
        Lock::GlobalLock lk(opCtx.get(), MODE_IS);

        // The RstlKillOpThread would only interrupt a read operation if the OperationContext opted
        // into always being interrupted.
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
        ASSERT_FALSE(shard_role_details::getLocker(opCtx.get())
                         ->wasGlobalLockTakenInModeConflictingWithWrites());

        auto [instance2, isPausedOrShutdown] =
            TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0));
        ASSERT_TRUE(instance2);
        ASSERT_FALSE(isPausedOrShutdown);
        ASSERT_EQ(instance.get(), instance2.value().get());
    }

    TestServiceHangDuringInitialization.setMode(FailPoint::off);
    instance->getCompletionFuture().get();
}

TEST_F(PrimaryOnlyServiceTest, LookupInstanceHoldingIXLock) {
    // Make sure the instance doesn't complete before we try to look it up.
    TestServiceHangDuringInitialization.setMode(FailPoint::alwaysOn);

    auto opCtx = makeOperationContext();
    auto instance =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
    ASSERT(instance.get());
    ASSERT_EQ(0, instance->getID());

    {
        Lock::GlobalLock lk(opCtx.get(), MODE_IX);
        ASSERT_FALSE(opCtx->shouldAlwaysInterruptAtStepDownOrUp());
        auto [instance2, isPausedOrShutdown] =
            TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0));
        ASSERT_TRUE(instance2);
        ASSERT_FALSE(isPausedOrShutdown);
        ASSERT_EQ(instance.get(), instance2.value().get());
    }

    TestServiceHangDuringInitialization.setMode(FailPoint::off);
    instance->getCompletionFuture().get();
}

DEATH_TEST_F(PrimaryOnlyServiceTest,
             LookupInstanceHoldingISLockWithoutAlwaysBeingInterruptible,
             "invariant") {
    // Make sure the instance doesn't complete before we try to look it up.
    TestServiceHangDuringInitialization.setMode(FailPoint::alwaysOn);

    auto opCtx = makeOperationContext();
    auto instance =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
    ASSERT(instance.get());
    ASSERT_EQ(0, instance->getID());

    {
        Lock::GlobalLock lk(opCtx.get(), MODE_IS);
        ASSERT_FALSE(opCtx->shouldAlwaysInterruptAtStepDownOrUp());
        ASSERT_FALSE(shard_role_details::getLocker(opCtx.get())
                         ->wasGlobalLockTakenInModeConflictingWithWrites());
        TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0));
    }
}

TEST_F(PrimaryOnlyServiceTest, LookupInstanceAfterStepDownReturnsNone) {
    // Make sure the instance doesn't complete before we try to look it up.
    auto timesEntered = TestServiceHangDuringInitialization.setMode(FailPoint::alwaysOn);

    auto opCtx = makeOperationContext();
    auto instance =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
    ASSERT(instance.get());
    ASSERT_EQ(0, instance->getID());

    // Wait for Instance::run() to be called before calling stepDown so that the _completionPromise
    // will eventually be set.
    TestServiceHangDuringInitialization.waitForTimesEntered(timesEntered + 1);

    stepDown();

    auto [instance2, isPausedOrShutdown] =
        TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0));

    ASSERT_FALSE(instance2);
    ASSERT_TRUE(isPausedOrShutdown);

    TestServiceHangDuringInitialization.setMode(FailPoint::off);
    ASSERT_EQ(ErrorCodes::Interrupted, instance->getCompletionFuture().getNoThrow());
}

TEST_F(PrimaryOnlyServiceTest, LookupInstanceAfterShutDownReturnsNone) {
    // Make sure the instance doesn't complete before we try to look it up.
    auto timesEntered = TestServiceHangDuringInitialization.setMode(FailPoint::alwaysOn);

    auto opCtx = makeOperationContext();
    auto instance =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
    ASSERT(instance.get());
    ASSERT_EQ(0, instance->getID());
    // Make sure we enter the run function before shutting down.
    TestServiceHangDuringInitialization.waitForTimesEntered(timesEntered + 1);
    TestServiceHangDuringInitialization.setMode(FailPoint::off);

    shutdown();

    auto [instance2, isPausedOrShutdown] =
        TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0));

    ASSERT_FALSE(instance2);
    ASSERT_TRUE(isPausedOrShutdown);

    ASSERT_EQ(ErrorCodes::Interrupted, instance->getCompletionFuture().getNoThrow());
}

TEST_F(PrimaryOnlyServiceTest, GetAllInstancesAfterStepDownReturnsEmptyVector) {
    // Make sure the instance doesn't complete before we try to look it up.
    auto timesEntered = TestServiceHangDuringInitialization.setMode(FailPoint::alwaysOn);

    auto opCtx = makeOperationContext();
    auto instance =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
    ASSERT(instance.get());
    ASSERT_EQ(0, instance->getID());

    // Wait for Instance::run() to be called before calling stepDown so that the _completionPromise
    // will eventually be set.
    TestServiceHangDuringInitialization.waitForTimesEntered(timesEntered + 1);

    stepDown();

    auto instances = static_cast<TestService*>(_service)->getAllInstances(opCtx.get());

    ASSERT_EQ(instances.size(), 0);

    TestServiceHangDuringInitialization.setMode(FailPoint::off);
    ASSERT_EQ(ErrorCodes::Interrupted, instance->getCompletionFuture().getNoThrow());
}

TEST_F(PrimaryOnlyServiceTest, GetAllInstancesAfterShutDownReturnsEmptyVector) {
    // Make sure the instance doesn't complete before we try to look it up.
    auto timesEntered = TestServiceHangDuringInitialization.setMode(FailPoint::alwaysOn);

    auto opCtx = makeOperationContext();
    auto instance =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
    ASSERT(instance.get());
    ASSERT_EQ(0, instance->getID());

    // Make sure we enter the run function before shutting down.
    TestServiceHangDuringInitialization.waitForTimesEntered(timesEntered + 1);
    TestServiceHangDuringInitialization.setMode(FailPoint::off);

    shutdown();

    auto instances = static_cast<TestService*>(_service)->getAllInstances(opCtx.get());

    ASSERT_EQ(instances.size(), 0);

    ASSERT_EQ(ErrorCodes::Interrupted, instance->getCompletionFuture().getNoThrow());
}

TEST_F(PrimaryOnlyServiceTest, GetOrCreateInstanceInterruptible) {
    stepDown();
    // Make sure the service stays in state kRebuilding so that getOrCreate() has to try to wait on
    // the opCtx for the state to change.
    PrimaryOnlyServiceHangBeforeRebuildingInstances.setMode(FailPoint::alwaysOn);
    stepUp();

    auto opCtx = makeOperationContext();
    opCtx->markKilled(ErrorCodes::Interrupted);
    ASSERT_THROWS_CODE(TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0)),
                       DBException,
                       ErrorCodes::Interrupted);
}

TEST_F(PrimaryOnlyServiceTest, DoubleCreateInstance) {
    // Make sure the first instance doesn't complete before we try to create the second.
    TestServiceHangDuringInitialization.setMode(FailPoint::alwaysOn);

    auto opCtx = makeOperationContext();
    auto instance =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
    ASSERT(instance.get());
    ASSERT_EQ(0, instance->getID());

    // Trying to create a new instance with the same _id but different state otherwise just returns
    // the already existing instance based on the _id only.
    auto instance2 =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0 << "state" << 1));
    ASSERT_EQ(instance.get(), instance2.get());

    TestServiceHangDuringInitialization.setMode(FailPoint::off);
}

TEST_F(PrimaryOnlyServiceTest, ReportServerStatusInfo) {
    stepDown();
    // Make the instance rebuild on stepUp hang.
    auto rebuildingFPTimesEntered =
        PrimaryOnlyServiceHangBeforeRebuildingInstances.setMode(FailPoint::alwaysOn);
    stepUp();

    {
        PrimaryOnlyServiceHangBeforeRebuildingInstances.waitForTimesEntered(
            ++rebuildingFPTimesEntered);

        BSONObjBuilder resultBuilder;
        _registry->reportServiceInfoForServerStatus(&resultBuilder);

        ASSERT_BSONOBJ_EQ(BSON("primaryOnlyServices"
                               << BSON("TestService" << BSON("state" << "rebuilding"
                                                                     << "numInstances" << 0))),
                          resultBuilder.obj());
    }

    // Make sure the instance doesn't complete.
    TestServiceHangDuringInitialization.setMode(FailPoint::alwaysOn);
    PrimaryOnlyServiceHangBeforeRebuildingInstances.setMode(FailPoint::off);

    auto opCtx = makeOperationContext();
    auto instance =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));

    {
        BSONObjBuilder resultBuilder;
        _registry->reportServiceInfoForServerStatus(&resultBuilder);

        ASSERT_BSONOBJ_EQ(BSON("primaryOnlyServices"
                               << BSON("TestService" << BSON("state" << "running"
                                                                     << "numInstances" << 1))),
                          resultBuilder.obj());
    }

    auto instance2 =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 1 << "state" << 0));

    {
        BSONObjBuilder resultBuilder;
        _registry->reportServiceInfoForServerStatus(&resultBuilder);

        ASSERT_BSONOBJ_EQ(BSON("primaryOnlyServices"
                               << BSON("TestService" << BSON("state" << "running"
                                                                     << "numInstances" << 2))),
                          resultBuilder.obj());
    }

    TestServiceHangDuringInitialization.setMode(FailPoint::off);
}

TEST_F(PrimaryOnlyServiceTest, CreateWhenNotPrimary) {
    _registry->onStepDown();
    auto opCtx = makeOperationContext();

    ASSERT_THROWS_CODE(
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0 << "state" << 0)),
        DBException,
        ErrorCodes::NotWritablePrimary);
}

TEST_F(PrimaryOnlyServiceTest, CreateWithoutID) {
    auto opCtx = makeOperationContext();
    ASSERT_THROWS_CODE(
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("state" << 0)),
        DBException,
        4908702);
}

TEST_F(PrimaryOnlyServiceTest, StepDownBeforePersisted) {
    // Prevent the instance from writing its initial state document to the storage engine.
    auto timesEntered = TestServiceHangDuringInitialization.setMode(FailPoint::alwaysOn);

    {
        auto opCtx = makeOperationContext();
        auto instance = TestService::Instance::getOrCreate(
            opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
        TestServiceHangDuringInitialization.waitForTimesEntered(++timesEntered);
        stepDown();
        TestServiceHangDuringInitialization.setMode(FailPoint::off);

        ASSERT_THROWS_CODE_AND_WHAT(instance->getCompletionFuture().get(),
                                    DBException,
                                    ErrorCodes::Interrupted,
                                    "Interrupted");
    }

    stepUp();

    auto opCtx = makeOperationContext();
    // Since the Instance never wrote its state document, it shouldn't be recreated on stepUp.
    auto [recreatedInstance, isPausedOrShutdown] =
        TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0));
    ASSERT_FALSE(recreatedInstance);
    ASSERT_FALSE(isPausedOrShutdown);
}

TEST_F(PrimaryOnlyServiceTest, RecreateInstanceOnStepUp) {
    // Cause the Instance to be interrupted after writing its initial state document in state 1.
    auto stateOneFPTimesEntered = TestServiceHangDuringStateOne.setMode(FailPoint::alwaysOn);

    {
        auto opCtx = makeOperationContext();
        auto instance = TestService::Instance::getOrCreate(
            opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
        TestServiceHangDuringStateOne.waitForTimesEntered(++stateOneFPTimesEntered);

        ASSERT_EQ(TestService::State::kInitializing, instance->getInitialState());
        ASSERT_EQ(TestService::State::kOne, instance->getState());
    }

    stepDown();

    TestServiceHangDuringStateOne.setMode(FailPoint::off);
    auto stateTwoFPTimesEntered = TestServiceHangDuringStateTwo.setMode(FailPoint::alwaysOn);

    stepUp();

    {
        auto opCtx = makeOperationContext();
        auto [recreatedInstance, isPausedOrShutdown] =
            TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0));
        ASSERT_TRUE(recreatedInstance);
        ASSERT_FALSE(isPausedOrShutdown);
        ASSERT_EQ(TestService::State::kOne, (*recreatedInstance)->getInitialState());
        TestServiceHangDuringStateTwo.waitForTimesEntered(++stateTwoFPTimesEntered);
        ASSERT_EQ(TestService::State::kTwo, (*recreatedInstance)->getState());
    }

    stepDown();

    TestServiceHangDuringStateTwo.setMode(FailPoint::off);
    // Need to block instance execution after it's started running but before it's completed so that
    // the lookup() call later can find the Instance.
    stateOneFPTimesEntered = TestServiceHangDuringStateOne.setMode(FailPoint::alwaysOn);

    stepUp();

    {
        auto opCtx = makeOperationContext();
        auto [recreatedInstance, isPausedOrShutdown1] =
            TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0));
        ASSERT_TRUE(recreatedInstance);
        ASSERT_FALSE(isPausedOrShutdown1);
        ASSERT_EQ(TestService::State::kTwo, (*recreatedInstance)->getInitialState());
        TestServiceHangDuringStateOne.setMode(FailPoint::off);
        (*recreatedInstance)->getCompletionFuture().get();
        ASSERT_EQ(TestService::State::kDone, (*recreatedInstance)->getState());

        auto [nonExistentInstance, isPausedOrShutdown2] =
            TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0));
        ASSERT_FALSE(nonExistentInstance);
        ASSERT_FALSE(isPausedOrShutdown2);
    }

    stepDown();
    stepUp();

    {
        auto opCtx = makeOperationContext();
        // No Instance should be recreated since the previous run completed successfully and deleted
        // its state document.
        auto [nonExistentInstance, isPausedOrShutdown] =
            TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0));
        ASSERT_FALSE(nonExistentInstance);
        ASSERT_FALSE(isPausedOrShutdown);
    }
}

TEST_F(PrimaryOnlyServiceTest, StepDownBeforeRebuildingInstances) {
    // Cause the Instance to be interrupted after writing its initial state document in state 1.
    auto stateOneFPTimesEntered = TestServiceHangDuringStateOne.setMode(FailPoint::alwaysOn);

    {
        auto opCtx = makeOperationContext();
        auto instance = TestService::Instance::getOrCreate(
            opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
        TestServiceHangDuringStateOne.waitForTimesEntered(++stateOneFPTimesEntered);

        ASSERT_EQ(TestService::State::kInitializing, instance->getInitialState());
        ASSERT_EQ(TestService::State::kOne, instance->getState());
    }

    stepDown();

    // Let the running instance terminate.
    TestServiceHangDuringStateOne.setMode(FailPoint::off);
    // Make the instance rebuild on stepUp hang
    auto rebuildingFPTimesEntered =
        PrimaryOnlyServiceHangBeforeRebuildingInstances.setMode(FailPoint::alwaysOn);

    stepUp();

    // Start an async task to lookup the rebuilt instance, this will block waiting on the rebuild to
    // finish.
    {
        auto getInstanceFuture =
            SemiFuture<void>::makeReady().thenRunOn(_testExecutor).then([this]() {
                auto opCtx = cc().makeOperationContext();
                return TestService::Instance::getOrCreate(
                    opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
            });
        PrimaryOnlyServiceHangBeforeRebuildingInstances.waitForTimesEntered(
            ++rebuildingFPTimesEntered);
        ASSERT_FALSE(getInstanceFuture.isReady());

        // Stepdown, interrupting the thread waiting for the rebuilt instance.
        stepDown();

        // Let the previous stepUp attempt continue and realize that the node has since stepped
        // down.
        PrimaryOnlyServiceHangBeforeRebuildingInstances.setMode(FailPoint::off);
        ASSERT_THROWS_CODE(getInstanceFuture.get(), DBException, ErrorCodes::NotWritablePrimary);
    }

    // Now do another stepUp that is allowed to complete this time.
    stateOneFPTimesEntered = TestServiceHangDuringStateOne.setMode(FailPoint::alwaysOn);
    stepUp();
    TestServiceHangDuringStateOne.waitForTimesEntered(++stateOneFPTimesEntered);

    auto opCtx = makeOperationContext();
    auto [instance, isPausedOrShutdown] =
        TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0));
    ASSERT_TRUE(instance);
    ASSERT_FALSE(isPausedOrShutdown);
    ASSERT_EQ(TestService::State::kOne, (*instance)->getInitialState());
    ASSERT_EQ(TestService::State::kOne, (*instance)->getState());

    TestServiceHangDuringStateOne.setMode(FailPoint::off);

    (*instance)->getCompletionFuture().get();
}

TEST_F(PrimaryOnlyServiceTest, RecreateInstancesFails) {
    // Cause the Instance to be interrupted after writing its initial state document in state 1.
    auto stateOneFPTimesEntered = TestServiceHangDuringStateOne.setMode(FailPoint::alwaysOn);

    {
        auto opCtx = makeOperationContext();
        auto instance = TestService::Instance::getOrCreate(
            opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
        TestServiceHangDuringStateOne.waitForTimesEntered(++stateOneFPTimesEntered);

        ASSERT_EQ(TestService::State::kInitializing, instance->getInitialState());
        ASSERT_EQ(TestService::State::kOne, instance->getState());
    }

    stepDown();

    TestServiceHangDuringStateOne.setMode(FailPoint::off);
    // Make querying the state document collection on stepUp fail
    PrimaryOnlyServiceFailRebuildingInstances.setMode(FailPoint::alwaysOn);

    stepUp();

    {
        auto opCtx = makeOperationContext();
        // Now that rebuilding the service on stepUp failed, all subsequent operations on that
        // service will fail until the next stepDown.
        ASSERT_THROWS_CODE(TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0)),
                           DBException,
                           ErrorCodes::InternalError);
        ASSERT_THROWS_CODE(TestService::Instance::getOrCreate(
                               opCtx.get(), _service, BSON("_id" << 0 << "state" << 0)),
                           DBException,
                           ErrorCodes::InternalError);
    }

    stepDown();

    {
        // After stepping down we are in a consistent state again, but cannot create or lookup
        // instances because we are not primary.
        auto opCtx = makeOperationContext();
        auto [instance, isPausedOrShutdown] =
            TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0));
        ASSERT_FALSE(instance);
        ASSERT_TRUE(isPausedOrShutdown);
        ASSERT_THROWS_CODE(TestService::Instance::getOrCreate(
                               opCtx.get(), _service, BSON("_id" << 0 << "state" << 0)),
                           DBException,
                           ErrorCodes::NotWritablePrimary);
    }

    // Allow the next stepUp to succeed.
    PrimaryOnlyServiceFailRebuildingInstances.setMode(FailPoint::off);
    stateOneFPTimesEntered = TestServiceHangDuringStateOne.setMode(FailPoint::alwaysOn);

    stepUp();
    TestServiceHangDuringStateOne.waitForTimesEntered(++stateOneFPTimesEntered);

    {
        // Instance should be recreated successfully.
        auto opCtx = makeOperationContext();
        auto [instance, isPausedOrShutdown] =
            TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0));
        ASSERT_TRUE(instance);
        ASSERT_FALSE(isPausedOrShutdown);
        ASSERT_EQ(TestService::State::kOne, (*instance)->getInitialState());
        ASSERT_EQ(TestService::State::kOne, (*instance)->getState());
        TestServiceHangDuringStateOne.setMode(FailPoint::off);
        (*instance)->getCompletionFuture().get();
        ASSERT_EQ(TestService::State::kDone, (*instance)->getState());
    }
}

TEST_F(PrimaryOnlyServiceTest, OpCtxCreatedAfterStepdownIsAlreadyInterrupted) {
    // Ensure that if work has already been scheduled on the executor, but hasn't yet created an
    // OpCtx, and then we stepDown, that the OpCtx that gets created still gets interrupted.
    auto timesEntered = TestServiceHangBeforeMakingOpCtx.setMode(FailPoint::alwaysOn);

    auto opCtx = makeOperationContext();
    auto instance =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
    TestServiceHangBeforeMakingOpCtx.waitForTimesEntered(++timesEntered);
    stepDown();
    ASSERT(!instance->getDocumentWriteException().isReady());
    TestServiceHangBeforeMakingOpCtx.setMode(FailPoint::off);

    ASSERT_EQ(ErrorCodes::Interrupted, instance->getCompletionFuture().getNoThrow());
    ASSERT_EQ(ErrorCodes::NotWritablePrimary, instance->getDocumentWriteException().getNoThrow());
}


TEST_F(PrimaryOnlyServiceTest, OpCtxInterruptedByStepdown) {
    // Ensure that OpCtxs for PrimaryOnlyService work get interrupted by stepDown.
    auto timesEntered = TestServiceHangAfterMakingOpCtx.setMode(FailPoint::alwaysOn);

    auto opCtx = makeOperationContext();
    auto instance =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
    TestServiceHangAfterMakingOpCtx.waitForTimesEntered(timesEntered + 1);
    stepDown();

    ASSERT_EQ(ErrorCodes::Interrupted, instance->getCompletionFuture().getNoThrow());
    ASSERT_EQ(ErrorCodes::InterruptedDueToReplStateChange,
              instance->getDocumentWriteException().getNoThrow());
}

TEST_F(PrimaryOnlyServiceTest, OpCtxInterruptedByShutdown) {
    // Ensure that OpCtxs for PrimaryOnlyService work get interrupted by shutdown.
    auto timesEntered = TestServiceHangAfterMakingOpCtx.setMode(FailPoint::alwaysOn);

    auto opCtx = makeOperationContext();
    auto instance =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
    TestServiceHangAfterMakingOpCtx.waitForTimesEntered(timesEntered + 1);
    shutdown();

    ASSERT_EQ(ErrorCodes::Interrupted, instance->getCompletionFuture().getNoThrow());
    ASSERT_EQ(ErrorCodes::InterruptedAtShutdown,
              instance->getDocumentWriteException().getNoThrow());
}

TEST_F(PrimaryOnlyServiceTest, ReportCurOpInfo) {
    // Make sure the instance doesn't complete before we try to report its state.
    TestServiceHangDuringInitialization.setMode(FailPoint::alwaysOn);

    auto opCtx = makeOperationContext();
    auto unreportedInstance =
        TestService::Instance::getOrCreate(opCtx.get(), _service, BSON("_id" << 0 << "state" << 0));
    auto reportedInstance = TestService::Instance::getOrCreate(
        opCtx.get(), _service, BSON("_id" << 1 << "state" << 0 << "reportOp" << true));

    std::vector<BSONObj> ops;
    _service->reportInstanceInfoForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode::kIncludeIdle,
        MongoProcessInterface::CurrentOpSessionsMode::kIncludeIdle,
        &ops);
    ASSERT_EQ(1, ops.size());
    ASSERT_EQ(1, ops[0]["instanceID"]["_id"].Int());
    ASSERT_TRUE(ops[0].hasField("state"));
}

TEST_F(PrimaryOnlyServiceTest, StateTransitionFromRebuildingShouldWakeUpConditionVariable) {
    /*
     * 1. `onStepUp()` changes `_state` to `kRebuilding`.
     * 2. `lookupInstance()` is called immediately and blocks on the condition variable, after the
     * initial predicate check returns `false` since `_state == kRebuilding`.
     * 3. `onStepDown()` is called and sets `_state` to `kPaused`. After changing the state, it
     * should notify waiters on the condition variable.
     * 4. `lookupInstance()` should return after receiving the notification from `onStepDown()`.
     */

    stepDown();

    stdx::thread stepUpThread;
    stdx::thread lookUpInstanceThread;

    {
        FailPointEnableBlock stepUpFailpoint("PrimaryOnlyServiceHangBeforeLaunchingStepUpLogic");
        stepUpThread = stdx::thread([this] {
            ThreadClient tc("StepUpThread", getServiceContext()->getService());
            stepUp();
        });

        stepUpFailpoint->waitForTimesEntered(stepUpFailpoint.initialTimesEntered() + 1);

        lookUpInstanceThread = stdx::thread([this] {
            ThreadClient tc("LookUpInstanceThread", getServiceContext()->getService());
            auto opCtx = makeOperationContext();
            TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0));
        });
        // This is a best effort to wait for the `waitForConditionOrInterrupt` in `lookupInstance`
        // to make the initial call to its predicate while `_state` is `kRebuilding`.
        sleepmillis(100);

        stepDown();
    }

    stepUpThread.join();
    lookUpInstanceThread.join();
}

TEST_F(PrimaryOnlyServiceTest, PrimaryOnlyServiceLogSlowServices) {
    primaryOnlyServiceTestStepUpWaitForRebuildComplete.setMode(FailPoint::alwaysOn);
    // We change slow stepUp threshold to a extremely large value to avoid test failure in some slow
    // test environments.
    const auto oldSlowServiceOnStepUpCompleteThresholdMS =
        repl::slowServiceOnStepUpCompleteThresholdMS.load();
    const auto oldSlowTotalOnStepUpCompleteThresholdMS =
        repl::slowTotalOnStepUpCompleteThresholdMS.load();
    repl::slowServiceOnStepUpCompleteThresholdMS.store(1000000);
    repl::slowTotalOnStepUpCompleteThresholdMS.store(1000000);
    std::string slowSingleServiceStepUpCompleteMsg =
        "Duration spent in PrimaryOnlyServiceRegistry::onStepUpComplete for service "
        "exceeded slowServiceOnStepUpCompleteThresholdMS";
    std::string slowTotalTimeStepUpCompleteMsg =
        "Duration spent in PrimaryOnlyServiceRegistry::onStepUpComplete for all services "
        "exceeded slowTotalOnStepUpCompleteThresholdMS";

    stepDown();

    // First test a stepUp with no hanging. We shouldn't log anything.
    unittest::LogCaptureGuard logs;
    stepUp();
    logs.stop();
    ASSERT_EQ(0, logs.countTextContaining(slowSingleServiceStepUpCompleteMsg));
    ASSERT_EQ(0, logs.countTextContaining(slowTotalTimeStepUpCompleteMsg));

    // Reset thresholds.
    repl::slowServiceOnStepUpCompleteThresholdMS.store(oldSlowServiceOnStepUpCompleteThresholdMS);
    repl::slowTotalOnStepUpCompleteThresholdMS.store(oldSlowTotalOnStepUpCompleteThresholdMS);

    // Use the "PrimaryOnlyServiceHangBeforeLaunchingStepUpLogic", which is encountered
    // via PrimaryOnlyServiceRegistry::onStepUpComplete, to simulate a slow primary-only
    // service.
    auto doStepUpWithHang = [this](long long ms) {
        stepDown();

        auto stepUpTimesEntered =
            PrimaryOnlyServiceHangBeforeLaunchingStepUpLogic.setMode(FailPoint::alwaysOn);

        // Start an async task to step up, which will block on the fail point.
        auto stepUpFuture = ExecutorFuture<void>(_testExecutor).then([this]() { stepUp(); });

        // Once stepUp has hit the failpoint, sleep for the specified amount of ms so the service
        // hangs, before disabling the failpoint and allowing stepUp to proceed.
        PrimaryOnlyServiceHangBeforeLaunchingStepUpLogic.waitForTimesEntered(++stepUpTimesEntered);
        sleepmillis(ms);
        PrimaryOnlyServiceHangBeforeLaunchingStepUpLogic.setMode(FailPoint::off);
        stepUpFuture.wait();
    };

    // Hang for long enough to generate a message about the single service being slow.
    logs.start();
    doStepUpWithHang(repl::slowServiceOnStepUpCompleteThresholdMS.load() + 1);
    logs.stop();
    ASSERT_EQ(1, logs.countTextContaining(slowSingleServiceStepUpCompleteMsg));

    // Hang for long enough to generate a message about the total time being slow.
    logs.start();
    doStepUpWithHang(repl::slowTotalOnStepUpCompleteThresholdMS.load() + 1);
    logs.stop();
    ASSERT_EQ(1, logs.countTextContaining(slowTotalTimeStepUpCompleteMsg));
}

TEST_F(PrimaryOnlyServiceTest, RebuildServiceFailsShouldSetStateFromRebuilding) {
    /**
     * (1) onStepUp changes state to kRebuilding. lookupInstanceThread blocks waiting for state
     * not rebuilding.
     * (2) PrimaryOnlyService::_rebuildService fails due to fail point, causing rebuilding to
     * fail, despite no stepDown/shutDown occurring.
     * (3) Failure _rebuildService results in state change to kRebuildFailed.
     * (4) lookupInstanceThread notified of the state change, unblocks and sees error that
     * caused _rebuildService to fail.
     */
    stepDown();
    stdx::thread stepUpThread;
    stdx::thread lookUpInstanceThread;
    Status lookupError = Status::OK();
    FailPointEnableBlock failRebuildServiceFailPoint("TestServiceFailRebuildService");

    {
        FailPointEnableBlock stepUpFailpoint("PrimaryOnlyServiceHangBeforeLaunchingStepUpLogic");
        stepUpThread = stdx::thread([this] {
            ThreadClient tc("StepUpThread", getServiceContext()->getService());
            stepUp();
        });

        stepUpFailpoint->waitForTimesEntered(stepUpFailpoint.initialTimesEntered() + 1);

        lookUpInstanceThread = stdx::thread([this, &lookupError] {
            try {
                ThreadClient tc("LookUpInstanceThread", getServiceContext()->getService());
                auto opCtx = makeOperationContext();
                TestService::Instance::lookup(opCtx.get(), _service, BSON("_id" << 0));
            } catch (DBException& ex) {
                lookupError = ex.toStatus();
            }
        });
    }

    failRebuildServiceFailPoint->waitForTimesEntered(
        failRebuildServiceFailPoint.initialTimesEntered() + 1);
    stepUpThread.join();
    lookUpInstanceThread.join();

    ASSERT(!lookupError.isOK()) << "lookup thread did not receive an error";
    ASSERT_EQ(lookupError.code(), ErrorCodes::InternalError);
    ASSERT_EQ(lookupError.reason(), "_rebuildService error");
}
