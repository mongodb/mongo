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

#include "mongo/db/global_catalog/ddl/migration_blocking_operation_coordinator.h"

#include "mongo/db/global_catalog/ddl/migration_blocking_operation_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_external_state_for_test.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

constexpr auto kHangBeforeUpdatingInMemory = "hangBeforeUpdatingInMemory";
constexpr auto kHangBeforeUpdatingDiskState = "hangBeforeUpdatingDiskState";
constexpr auto kHangBeforeBlockingMigrations = "hangBeforeBlockingMigrations";
constexpr auto kHangBeforeAllowingMigrations = "hangBeforeAllowingMigrations";
constexpr auto kHangBeforeFulfillingPromise = "hangBeforeFulfillingPromise";
constexpr auto kHangBeforeRemovingCoordinatorDocument = "hangBeforeRemovingCoordinatorDocument";

class MigrationBlockingOperationCoordinatorTest : public repl::PrimaryOnlyServiceMongoDTest {
protected:
    using Service = ShardingDDLCoordinatorService;
    using Instance = MigrationBlockingOperationCoordinator;

    const NamespaceString kNamespace =
        NamespaceString::createNamespaceString_forTest("testDb", "coll");
    const DatabaseVersion kDbVersion{UUID::gen(), Timestamp(1, 0)};

    MigrationBlockingOperationCoordinatorTest() {
        _externalState = std::make_shared<ShardingDDLCoordinatorExternalStateForTest>();
        _externalStateFactory =
            std::make_unique<ShardingDDLCoordinatorExternalStateFactoryForTest>(_externalState);
    }

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<Service>(serviceContext, std::move(_externalStateFactory));
    }

    ShardingDDLCoordinatorId getCoordinatorId() const {
        return ShardingDDLCoordinatorId{kNamespace,
                                        DDLCoordinatorTypeEnum::kMigrationBlockingOperation};
    }

    ShardingDDLCoordinatorMetadata createMetadata() const {
        ShardingDDLCoordinatorMetadata metadata(getCoordinatorId());
        metadata.setForwardableOpMetadata(ForwardableOperationMetadata(_opCtx));
        metadata.setDatabaseVersion(kDbVersion);
        return metadata;
    }

    MigrationBlockingOperationCoordinatorDocument createStateDocument() const {
        MigrationBlockingOperationCoordinatorDocument doc;
        auto metadata = createMetadata();
        doc.setShardingDDLCoordinatorMetadata(metadata);
        return doc;
    }

    void setUp() override {
        PrimaryOnlyServiceMongoDTest::setUp();

        _opCtxHolder = makeOperationContext();
        _opCtx = _opCtxHolder.get();
        auto stateDocument = createStateDocument();
        _instance = checked_pointer_cast<MigrationBlockingOperationCoordinator>(
            Instance::getOrCreate(_opCtx, _service, stateDocument.toBSON()));
    }

    bool stateDocumentExistsOnDisk() {
        DBDirectClient client(_opCtx);
        auto count = client.count(NamespaceString::kShardingDDLCoordinatorsNamespace,
                                  BSON("_id" << getCoordinatorId().toBSON()));
        return count > 0;
    }

    MigrationBlockingOperationCoordinatorDocument getStateDocumentOnDisk() {
        ASSERT_TRUE(stateDocumentExistsOnDisk());
        DBDirectClient client(_opCtx);
        auto doc = client.findOne(NamespaceString::kShardingDDLCoordinatorsNamespace,
                                  BSON("_id" << getCoordinatorId().toBSON()));
        IDLParserContext errCtx(
            "MigrationBlockingOperationCoordinatorTest::getStateDocumentOnDisk()");
        return MigrationBlockingOperationCoordinatorDocument::parse(doc, errCtx);
    }

    void assertOperationCountOnDisk(int expectedCount) {
        auto doc = getStateDocumentOnDisk();
        ASSERT_EQ(expectedCount, doc.getOperations().get().size());
    }

    void beginOperations() {
        for (const auto& operationId : _operations) {
            _instance->beginOperation(_opCtx, operationId);
        }

        ASSERT_TRUE(stateDocumentExistsOnDisk());
    }

    void endOperations() {
        for (const auto& operationId : _operations) {
            _instance->endOperation(_opCtx, operationId);
        }
    }

    void tearDown() override {
        if (stateDocumentExistsOnDisk()) {
            auto doc = getStateDocumentOnDisk();
            for (const auto& operationId : doc.getOperations().get()) {
                _instance->endOperation(_opCtx, operationId);
            }
        }

        ASSERT_OK(_instance->getCompletionFuture().getNoThrow());
        ASSERT_FALSE(stateDocumentExistsOnDisk());
        PrimaryOnlyServiceMongoDTest::tearDown();
    }

    std::shared_ptr<Instance> getExistingInstance() {
        auto instanceId = BSON("_id" << getCoordinatorId().toBSON());
        auto [maybeInstance, isPausedOrShutdown] = Instance::lookup(_opCtx, _service, instanceId);
        ASSERT_TRUE(maybeInstance);
        ASSERT_FALSE(isPausedOrShutdown);

        return checked_pointer_cast<MigrationBlockingOperationCoordinator>(*maybeInstance);
    }

    void assertNoExistingInstance() {
        auto instanceId = BSON("_id" << getCoordinatorId().toBSON());
        auto [maybeInstance, isPausedOrShutdown] = Instance::lookup(_opCtx, _service, instanceId);
        ASSERT_FALSE(maybeInstance);
    }

    void appendAllowMigrationsFailureResponse() {
        _externalState->allowMigrationsResponse.appendResponse(
            Fault(Status(ErrorCodes::HostUnreachable, "Simulated network error")));
    }

    void appendMigrationsAllowedFailureResponse() {
        _externalState->allowMigrationsResponse.appendResponse(
            Fault(Status(ErrorCodes::PrimarySteppedDown, "Current primary is stepped down")));
    }

    stdx::thread startBackgroundThread(std::function<void(OperationContext*)>&& fn) {
        return stdx::thread([=, this] {
            ThreadClient tc("backgroundTask", getServiceContext()->getService());
            auto sideOpCtx = tc->makeOperationContext();
            fn(sideOpCtx.get());
        });
    }

    void runBackgroundTaskAndSimulateFailover(const std::string& failpointName,
                                              std::function<void(OperationContext*)>&& fn) {
        auto fp = globalFailPointRegistry().find(failpointName);
        auto timesEntered = fp->setMode(FailPoint::alwaysOn);

        auto taskThread = startBackgroundThread(std::move(fn));

        fp->waitForTimesEntered(timesEntered + 1);
        stepDown();
        fp->setMode(FailPoint::off);

        ASSERT_NOT_OK(_instance->getCompletionFuture().getNoThrow());
        taskThread.join();
        stepUp(_opCtx);
    }

    void testBeginOpFailover(const std::string& failpointName) {
        runBackgroundTaskAndSimulateFailover(failpointName, [&](OperationContext* sideOpCtx) {
            ASSERT_THROWS(_instance->beginOperation(sideOpCtx, _operations[0]), DBException);
        });
    }

    void testEndOpFailoverAndRetry(std::string failpointName) {
        beginOperations();
        runBackgroundTaskAndSimulateFailover(failpointName, [&](OperationContext* sideOpCtx) {
            ASSERT_THROWS(_instance->endOperation(sideOpCtx, _operations[0]), DBException);
        });

        _instance = getExistingInstance();
        ASSERT_DOES_NOT_THROW(_instance->endOperation(_opCtx, _operations[0]));
    }

    std::shared_ptr<MigrationBlockingOperationCoordinator> _instance;
    ServiceContext::UniqueOperationContext _opCtxHolder;
    OperationContext* _opCtx;
    std::vector<UUID> _operations;
    std::unique_ptr<ShardingDDLCoordinatorExternalStateFactoryForTest> _externalStateFactory;
    std::shared_ptr<ShardingDDLCoordinatorExternalStateForTest> _externalState;
};

TEST_F(MigrationBlockingOperationCoordinatorTest, CreateAndDeleteStateDocument) {
    ASSERT_FALSE(stateDocumentExistsOnDisk());

    _operations = {UUID::gen()};
    beginOperations();
    assertOperationCountOnDisk(1);
    endOperations();

    ASSERT_OK(_instance->getCompletionFuture().getNoThrow());
    ASSERT_FALSE(stateDocumentExistsOnDisk());
}

TEST_F(MigrationBlockingOperationCoordinatorTest, EndOperationDecrementsCount) {
    _operations = {UUID::gen(), UUID::gen()};
    beginOperations();

    _instance->endOperation(_opCtx, _operations[0]);
    assertOperationCountOnDisk(1);
}

TEST_F(MigrationBlockingOperationCoordinatorTest, BeginMultipleOperations) {
    _operations = {UUID::gen(), UUID::gen()};
    beginOperations();
    assertOperationCountOnDisk(2);
}

TEST_F(MigrationBlockingOperationCoordinatorTest, BeginSameOperationMultipleTimes) {
    _operations = {UUID::gen()};
    beginOperations();

    beginOperations();
    assertOperationCountOnDisk(1);
}

TEST_F(MigrationBlockingOperationCoordinatorTest, EndSameOperationMultipleTimes) {
    _operations = {UUID::gen(), UUID::gen()};
    beginOperations();

    _instance->endOperation(_opCtx, _operations[0]);
    _instance->endOperation(_opCtx, _operations[0]);

    assertOperationCountOnDisk(1);
}

DEATH_TEST_F(MigrationBlockingOperationCoordinatorTest,
             InvalidInitialStateDocument,
             "Operations should not be ongoing while migrations are running") {
    auto stateDocument = createStateDocument();

    std::vector<UUID> operations = {UUID::gen()};
    stateDocument.setOperations(operations);

    // Trigger a stepDown/stepUp to clear the default instance created in setUp().
    stepDown();
    stepUp(_opCtx);

    Instance::getOrCreate(_opCtx, _service, stateDocument.toBSON());
}

DEATH_TEST_F(MigrationBlockingOperationCoordinatorTest,
             DuplicateOperationsOnDisk,
             "Duplicate operations found on disk with same UUID") {
    auto stateDocument = createStateDocument();
    stateDocument.setPhase(MigrationBlockingOperationCoordinatorPhaseEnum::kBlockingMigrations);

    auto duplicateUUID = UUID::gen();
    std::vector<UUID> duplicateOperationVector = {duplicateUUID, duplicateUUID};
    stateDocument.setOperations(duplicateOperationVector);

    // Trigger a stepDown/stepUp to clear the default instance created in setUp().
    stepDown();
    stepUp(_opCtx);

    Instance::getOrCreate(_opCtx, _service, stateDocument.toBSON());
}

TEST_F(MigrationBlockingOperationCoordinatorTest, BeginOperationThrowsWhileCoordinatorCleaningUp) {
    auto fp = globalFailPointRegistry().find(kHangBeforeRemovingCoordinatorDocument);
    invariant(fp);
    auto timesEntered = fp->setMode(FailPoint::alwaysOn);

    _operations = {UUID::gen()};
    beginOperations();

    auto taskThread = startBackgroundThread(
        [&](OperationContext* sideOpCtx) { _instance->endOperation(sideOpCtx, _operations[0]); });

    fp->waitForTimesEntered(timesEntered + 1);
    auto taskThread2 = stdx::thread([&] {
        ASSERT_THROWS_CODE(beginOperations(),
                           DBException,
                           ErrorCodes::MigrationBlockingOperationCoordinatorCleaningUp);
    });
    fp->setMode(FailPoint::off);

    taskThread.join();
    taskThread2.join();
    ASSERT_OK(_instance->getCompletionFuture().getNoThrow());
    ASSERT_FALSE(stateDocumentExistsOnDisk());
}

TEST_F(MigrationBlockingOperationCoordinatorTest,
       BeginOperationFailsBlockingMigrationsOnNetworkError) {
    _operations = {UUID::gen()};

    appendAllowMigrationsFailureResponse();
    ASSERT_THROWS_CODE(beginOperations(), DBException, ErrorCodes::HostUnreachable);

    // Ensure that the doc state was deleted and instance was clean up.
    ASSERT_OK(_instance->getCompletionFuture().getNoThrow());
    ASSERT_FALSE(stateDocumentExistsOnDisk());
}

TEST_F(MigrationBlockingOperationCoordinatorTest, EnsureEndOperationFailsAllowingMigrations) {
    _operations = {UUID::gen()};
    beginOperations();

    appendAllowMigrationsFailureResponse();
    ASSERT_THROWS_CODE(endOperations(), DBException, ErrorCodes::HostUnreachable);

    // Ensure that the doc state was untouched.
    ASSERT_TRUE(stateDocumentExistsOnDisk());
    assertOperationCountOnDisk(1);
}

TEST_F(MigrationBlockingOperationCoordinatorTest, FailoverBeforeBeginOpUpdatesInMemory) {
    _operations = {UUID::gen()};
    testBeginOpFailover(kHangBeforeUpdatingInMemory);

    assertNoExistingInstance();
    _instance = checked_pointer_cast<MigrationBlockingOperationCoordinator>(
        Instance::getOrCreate(_opCtx, _service, createStateDocument().toBSON()));
    ASSERT_DOES_NOT_THROW(beginOperations());

    ASSERT_FALSE(_externalState->migrationsAllowed);
}

TEST_F(MigrationBlockingOperationCoordinatorTest, FailoverBeforeBeginOpUpdatesDisk) {
    _operations = {UUID::gen()};
    testBeginOpFailover(kHangBeforeUpdatingDiskState);

    assertNoExistingInstance();
    _instance = checked_pointer_cast<MigrationBlockingOperationCoordinator>(
        Instance::getOrCreate(_opCtx, _service, createStateDocument().toBSON()));
    ASSERT_DOES_NOT_THROW(beginOperations());

    ASSERT_FALSE(_externalState->migrationsAllowed);
}

TEST_F(MigrationBlockingOperationCoordinatorTest, FailoverBeforeBeginOpBlocksMigrations) {
    _operations = {UUID::gen()};
    appendMigrationsAllowedFailureResponse();

    testBeginOpFailover(kHangBeforeBlockingMigrations);
    _instance = getExistingInstance();
    ASSERT_DOES_NOT_THROW(beginOperations());

    ASSERT_FALSE(_externalState->migrationsAllowed);
}

TEST_F(MigrationBlockingOperationCoordinatorTest, FailoverBeforeEndOpUpdatesInMemory) {
    _operations = {UUID::gen()};
    testEndOpFailoverAndRetry(kHangBeforeUpdatingInMemory);
    ASSERT_TRUE(_externalState->migrationsAllowed);
}

TEST_F(MigrationBlockingOperationCoordinatorTest, FailoverBeforeEndOpAllowsMigrations) {
    _operations = {UUID::gen()};
    testEndOpFailoverAndRetry(kHangBeforeAllowingMigrations);
    ASSERT_TRUE(_externalState->migrationsAllowed);
}

TEST_F(MigrationBlockingOperationCoordinatorTest, FailoverBeforeEndOpUpdatesDiskState) {
    _operations = {UUID::gen(), UUID::gen()};
    testEndOpFailoverAndRetry(kHangBeforeUpdatingDiskState);
    assertOperationCountOnDisk(1);
}

TEST_F(MigrationBlockingOperationCoordinatorTest, FailoverBeforeEndOpFulfillsPromise) {
    _operations = {UUID::gen()};
    testEndOpFailoverAndRetry(kHangBeforeFulfillingPromise);
    ASSERT_TRUE(_externalState->migrationsAllowed);
}

TEST_F(MigrationBlockingOperationCoordinatorTest, FailoverBeforeEndOpCleansUpStateDocument) {
    _operations = {UUID::gen()};
    testEndOpFailoverAndRetry(kHangBeforeRemovingCoordinatorDocument);
    ASSERT_TRUE(_externalState->migrationsAllowed);
}

TEST_F(MigrationBlockingOperationCoordinatorTest, TestBeginOpRecoveryWithMultipleCalls) {
    _operations = {UUID::gen()};
    appendMigrationsAllowedFailureResponse();

    testBeginOpFailover(kHangBeforeBlockingMigrations);
    _instance = getExistingInstance();
    ASSERT_DOES_NOT_THROW(_instance->beginOperation(_opCtx, UUID::gen()));

    ASSERT_FALSE(_externalState->migrationsAllowed);
    assertOperationCountOnDisk(2);
}

TEST_F(MigrationBlockingOperationCoordinatorTest, endOperationThrowsWhileSteppingDown) {
    auto fp = globalFailPointRegistry().find(kHangBeforeRemovingCoordinatorDocument);
    invariant(fp);
    auto timesEntered = fp->setMode(FailPoint::alwaysOn);

    _operations = {UUID::gen()};
    beginOperations();

    auto taskThread = startBackgroundThread([&](OperationContext* sideOpCtx) {
        ASSERT_THROWS(_instance->endOperation(sideOpCtx, _operations[0]), DBException);
    });

    fp->waitForTimesEntered(timesEntered + 1);
    stepDown();
    ASSERT_THROWS(endOperations(), DBException);
    fp->setMode(FailPoint::off);

    taskThread.join();
    stepUp(_opCtx);
    _instance = getExistingInstance();
}

}  // namespace
}  // namespace mongo
