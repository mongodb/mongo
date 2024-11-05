/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/s/ddl_lock_manager.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/migration_blocking_operation/migration_blocking_operation_coordinator.h"
#include "mongo/db/s/sharding_ddl_coordinator_external_state_for_test.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

class ShardingDDLCoordinatorServiceTest : public repl::PrimaryOnlyServiceMongoDTest {
public:
    ShardingDDLCoordinatorServiceTest() {
        _externalState = std::make_shared<ShardingDDLCoordinatorExternalStateForTest>();
        _externalStateFactory =
            std::make_unique<ShardingDDLCoordinatorExternalStateFactoryForTest>(_externalState);
    }

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<ShardingDDLCoordinatorService>(serviceContext,
                                                               std::move(_externalStateFactory));
    }

    void setUp() override {
        PrimaryOnlyServiceMongoDTest::setUp();
        _testExecutor = makeTestExecutor();

        DDLLockManager::get(getServiceContext())->setRecoverable(ddlService());
    }

    void tearDown() override {
        // Ensure that even on test failures all failpoint state gets reset.
        globalFailPointRegistry().disableAllFailpoints();

        _testExecutor->shutdown();
        _testExecutor->join();
        _testExecutor.reset();

        PrimaryOnlyServiceMongoDTest::tearDown();
    }

    ShardingDDLCoordinatorService* ddlService() {
        return static_cast<ShardingDDLCoordinatorService*>(_service);
    }

    std::shared_ptr<executor::TaskExecutor> makeTestExecutor() {
        ThreadPool::Options threadPoolOptions;
        threadPoolOptions.maxThreads = 1;
        threadPoolOptions.threadNamePrefix = "ShardingDDLCoordinatorServiceTest-";
        threadPoolOptions.poolName = "ShardingDDLCoordinatorServiceTestThreadPool";
        threadPoolOptions.onCreateThread = [](const std::string& threadName) {
            Client::initThread(threadName.c_str(), getGlobalServiceContext()->getService());
        };

        auto executor = executor::ThreadPoolTaskExecutor::create(
            std::make_unique<ThreadPool>(threadPoolOptions),
            executor::makeNetworkInterface(
                "ShardingDDLCoordinatorServiceTestNetwork", nullptr, nullptr));
        executor->startup();
        return executor;
    }

    void printState() {
        std::string stateStr;
        switch (ddlService()->_state) {
            case ShardingDDLCoordinatorService::State::kPaused:
                stateStr = "kPaused";
                break;
            case ShardingDDLCoordinatorService::State::kRecovered:
                stateStr = "kRecovered";
                break;
            case ShardingDDLCoordinatorService::State::kRecovering:
                stateStr = "kRecovering";
                break;
            default:
                MONGO_UNREACHABLE;
        }
        LOGV2(7646301, "ShardingDDLCoordinatorService::_state", "state"_attr = stateStr);
    }

    void assertStateIsPaused() {
        ASSERT_EQ(ShardingDDLCoordinatorService::State::kPaused, ddlService()->_state);
    }

    void assertStateIsRecovered() {
        ASSERT_EQ(ShardingDDLCoordinatorService::State::kRecovered, ddlService()->_state);
    }

protected:
    using ScopedBaseDDLLock = DDLLockManager::ScopedBaseDDLLock;

    /**
     * Acquire Database and Collection DDL locks on the given resource.
     */
    std::pair<ScopedBaseDDLLock, ScopedBaseDDLLock> acquireDbAndCollDDLLocks(
        OperationContext* opCtx,
        const NamespaceString& ns,
        StringData reason,
        LockMode mode,
        int32_t timeoutMillisecs,
        bool waitForRecovery = true) {

        FailPointEnableBlock fp("overrideDDLLockTimeout",
                                BSON("timeoutMillisecs" << timeoutMillisecs));
        return std::make_pair(
            ScopedBaseDDLLock{opCtx,
                              shard_role_details::getLocker(opCtx),
                              ns.dbName(),
                              reason,
                              mode,
                              waitForRecovery},
            ScopedBaseDDLLock{
                opCtx, shard_role_details::getLocker(opCtx), ns, reason, mode, waitForRecovery});
    }

    /**
     * Acquire Database and Collection DDL locks on the given resource without waiting for recovery
     * state to simulate requests coming from ShardingDDLCoordinators.
     */
    std::pair<ScopedBaseDDLLock, ScopedBaseDDLLock>
    acquireDbAndCollDDLLocksWithoutWaitingForRecovery(OperationContext* opCtx,
                                                      const NamespaceString& ns,
                                                      StringData reason,
                                                      LockMode mode,
                                                      int32_t timeoutMillisecs) {
        return acquireDbAndCollDDLLocks(
            opCtx, ns, reason, mode, timeoutMillisecs, false /*waitForRecovery*/);
    }

    MigrationBlockingOperationCoordinatorDocument createMBOCDoc(OperationContext* opCtx) {
        const auto coordinatorId = ShardingDDLCoordinatorId{
            NamespaceString::createNamespaceString_forTest("testDb", "coll"),
            DDLCoordinatorTypeEnum::kMigrationBlockingOperation};
        ShardingDDLCoordinatorMetadata metadata(coordinatorId);
        metadata.setForwardableOpMetadata(ForwardableOperationMetadata(opCtx));
        metadata.setDatabaseVersion(DatabaseVersion{UUID::gen(), Timestamp(1, 0)});
        MigrationBlockingOperationCoordinatorDocument doc;
        doc.setShardingDDLCoordinatorMetadata(metadata);
        return doc;
    }

    std::shared_ptr<executor::TaskExecutor> _testExecutor;
    std::unique_ptr<ShardingDDLCoordinatorExternalStateFactoryForTest> _externalStateFactory;
    std::shared_ptr<ShardingDDLCoordinatorExternalStateForTest> _externalState;
};

TEST_F(ShardingDDLCoordinatorServiceTest, StateTransitions) {
    auto opCtx = makeOperationContext();

    // Reaching a steady state to start the test
    ddlService()->waitForRecovery(opCtx.get());
    assertStateIsRecovered();

    // State must be `kPaused` after stepping down
    stepDown();
    assertStateIsPaused();

    // Check state is `kRecovered` once the recovery finishes
    stepUp(opCtx.get());
    ddlService()->waitForRecovery(opCtx.get());
    assertStateIsRecovered();
}

TEST_F(ShardingDDLCoordinatorServiceTest,
       DDLLocksCanOnlyBeAcquiredOnceShardingDDLCoordinatorServiceIsRecovered) {
    auto opCtx = makeOperationContext();
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Reaching a steady state to start the test
    ddlService()->waitForRecovery(opCtx.get());

    const std::string reason = "dummyReason";
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.coll");

    // 1- Stepping down
    // Only DDL coordinators can acquire DDL locks after stepping down, otherwise trying to acquire
    // a DDL lock will throw a LockTimeout error
    stepDown();

    ASSERT_THROWS_CODE(
        acquireDbAndCollDDLLocks(opCtx.get(), nss, reason, MODE_X, 0 /*timeoutMillisec*/),
        DBException,
        ErrorCodes::LockTimeout);

    ASSERT_DOES_NOT_THROW(acquireDbAndCollDDLLocksWithoutWaitingForRecovery(
        opCtx.get(), nss, reason, MODE_X, 0 /*timeoutMillisec*/));

    // 2- Stepping up and pausing on Recovery state
    // Only DDL coordinators can acquire DDL locks during recovery, otherwise trying to acquire a
    // DDL lock will throw a LockTimeout error
    auto pauseOnRecoveryFailPoint =
        globalFailPointRegistry().find("pauseShardingDDLCoordinatorServiceOnRecovery");
    const auto fpCount = pauseOnRecoveryFailPoint->setMode(FailPoint::alwaysOn);
    stepUp(opCtx.get());
    pauseOnRecoveryFailPoint->waitForTimesEntered(fpCount + 1);

    ASSERT_THROWS_CODE(
        acquireDbAndCollDDLLocks(opCtx.get(), nss, reason, MODE_X, 10 /*timeoutMillisec*/),
        DBException,
        ErrorCodes::LockTimeout);
    ASSERT_DOES_NOT_THROW(acquireDbAndCollDDLLocksWithoutWaitingForRecovery(
        opCtx.get(), nss, reason, MODE_X, 0 /*timeoutMillisec*/));

    // 3- Ending Recovery and enter on Recovered state
    // Once ShardingDDLCoordinatorService is recovered, anyone can aquire a DDL lock
    pauseOnRecoveryFailPoint->setMode(FailPoint::off);
    ddlService()->waitForRecovery(opCtx.get());

    ASSERT_DOES_NOT_THROW(
        acquireDbAndCollDDLLocks(opCtx.get(), nss, reason, MODE_X, 10 /*timeoutMillisec*/));
    ASSERT_DOES_NOT_THROW(acquireDbAndCollDDLLocksWithoutWaitingForRecovery(
        opCtx.get(), nss, reason, MODE_X, 0 /*timeoutMillisec*/));
}

TEST_F(ShardingDDLCoordinatorServiceTest, DDLLockMustBeEventuallyAcquiredAfterAStepUp) {
    auto opCtx = makeOperationContext();
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Reaching a steady state to start the test
    ddlService()->waitForRecovery(opCtx.get());

    const std::string reason = "dummyReason";
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.coll");

    stepDown();

    ASSERT_THROWS_CODE(
        acquireDbAndCollDDLLocks(opCtx.get(), nss, reason, MODE_X, 10 /*timeoutMillisec*/),
        DBException,
        ErrorCodes::NotWritablePrimary);

    unittest::Barrier syncPoint{2};

    // Start an async task to step up
    auto stepUpFuture = ExecutorFuture<void>(_testExecutor).then([this, &syncPoint]() {
        auto pauseOnRecoveryFailPoint =
            globalFailPointRegistry().find("pauseShardingDDLCoordinatorServiceOnRecovery");
        const auto fpCount = pauseOnRecoveryFailPoint->setMode(FailPoint::alwaysOn);


        auto opCtx = makeOperationContext();
        stepUp(opCtx.get());

        syncPoint.countDownAndWait();
        // Stay on recovery state for some time to ensure the lock is acquired before transition to
        // recovered state
        sleepFor(Milliseconds(30));
        pauseOnRecoveryFailPoint->waitForTimesEntered(fpCount + 1);
        pauseOnRecoveryFailPoint->setMode(FailPoint::off);
    });

    syncPoint.countDownAndWait();
    ASSERT_DOES_NOT_THROW(
        acquireDbAndCollDDLLocks(opCtx.get(), nss, reason, MODE_X, 10000 /*timeoutMillisec*/));

    // Lock should be acquired after step up conclusion
    ASSERT(stepUpFuture.isReady());
}

TEST_F(ShardingDDLCoordinatorServiceTest, CoordinatorCreationMustFailOnSecondaries) {
    auto opCtx = makeOperationContext();

    // Reaching a steady state to start the test
    ddlService()->waitForRecovery(opCtx.get());

    stepDown();

    ASSERT_THROWS_CODE(ddlService()->getOrCreateInstance(opCtx.get(), BSONObj()),
                       DBException,
                       ErrorCodes::NotWritablePrimary);

    ASSERT_THROWS_CODE(
        ddlService()->waitForRecovery(opCtx.get()), DBException, ErrorCodes::NotWritablePrimary);
}

TEST_F(ShardingDDLCoordinatorServiceTest, StepdownDuringServiceRebuilding) {
    auto opCtx = makeOperationContext();

    // Reaching a steady state to start the test
    ddlService()->waitForRecovery(opCtx.get());

    stepDown();

    auto pauseOnRecoveryFailPoint =
        globalFailPointRegistry().find("pauseShardingDDLCoordinatorServiceOnRecovery");
    const auto fpCount = pauseOnRecoveryFailPoint->setMode(FailPoint::alwaysOn);

    stepUp(opCtx.get());

    pauseOnRecoveryFailPoint->waitForTimesEntered(fpCount + 1);

    stepDown();

    pauseOnRecoveryFailPoint->setMode(FailPoint::off);

    stepUp(opCtx.get());

    // Reaching a steady state to start the test
    ddlService()->waitForRecovery(opCtx.get());
}

TEST_F(ShardingDDLCoordinatorServiceTest, StepdownStepupWhileCreatingCoordinator) {
    auto opCtx = makeOperationContext();

    MigrationBlockingOperationCoordinator::getOrCreate(
        opCtx.get(), ddlService(), createMBOCDoc(opCtx.get()).toBSON());

    for (size_t i = 0u; i < 10u; ++i) {
        stepDown();
        stepUp(opCtx.get());
    }

    ddlService()->waitForRecovery(opCtx.get());
}

}  // namespace mongo
