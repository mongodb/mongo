// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/migration_blocking_operation_coordinator_v2.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/migration_blocking_operation_coordinator_v2_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_external_state_for_test.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

constexpr auto kHangBeforeBlockingMigrations = "hangBeforeBlockingMigrationsV2";
constexpr auto kHangBeforeAllowingMigrations = "hangBeforeAllowingMigrationsV2";

class MigrationBlockingOperationCoordinatorV2Test : public repl::PrimaryOnlyServiceMongoDTest {
protected:
    using Service = ShardingCoordinatorService;
    using Instance = MigrationBlockingOperationCoordinatorV2;

    const NamespaceString kNamespace =
        NamespaceString::createNamespaceString_forTest("testDb", "coll");
    const DatabaseVersion kDbVersion{UUID::gen(), Timestamp(1, 0)};

    MigrationBlockingOperationCoordinatorV2Test() {
        _externalState = std::make_shared<ShardingCoordinatorExternalStateForTest>();
        _externalStateFactory =
            std::make_unique<ShardingCoordinatorExternalStateFactoryForTest>(_externalState);
    }

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<Service>(serviceContext, std::move(_externalStateFactory));
    }

    ShardingCoordinatorId getCoordinatorId() const {
        return ShardingCoordinatorId{kNamespace,
                                     CoordinatorTypeEnum::kMigrationBlockingOperationV2};
    }

    ShardingCoordinatorMetadata createMetadata() const {
        ShardingCoordinatorMetadata metadata(getCoordinatorId());
        metadata.setForwardableOpMetadata(ForwardableOperationMetadata(_opCtx));
        metadata.setDatabaseVersion(kDbVersion);
        metadata.setAuthoritativeMetadataAccessLevel(
            AuthoritativeMetadataAccessLevelEnum::kWritesAndReadsAllowed);
        return metadata;
    }

    MigrationBlockingOperationCoordinatorV2Document createStateDocument() const {
        MigrationBlockingOperationCoordinatorV2Document doc;
        auto metadata = createMetadata();
        doc.setShardingCoordinatorMetadata(metadata);
        return doc;
    }

    void setUp() override {
        PrimaryOnlyServiceMongoDTest::setUp();

        _opCtxHolder = makeOperationContext();
        _opCtx = _opCtxHolder.get();
    }

    // Creating the instance eagerly kicks off the migration-blocking work asynchronously.
    std::shared_ptr<Instance> getInstance() {
        return checked_pointer_cast<Instance>(
            Instance::getOrCreate(_opCtx, _service, createStateDocument().toBSON()));
    }

    std::shared_ptr<Instance> getExistingInstance() {
        auto instanceId = BSON("_id" << getCoordinatorId().toBSON());
        auto [maybeInstance, isPausedOrShutdown] = Instance::lookup(_opCtx, _service, instanceId);
        ASSERT_TRUE(maybeInstance);
        ASSERT_FALSE(isPausedOrShutdown);
        return checked_pointer_cast<Instance>(*maybeInstance);
    }

    bool stateDocumentExistsOnDisk() {
        DBDirectClient client(_opCtx);
        auto count = client.count(NamespaceString::kShardingDDLCoordinatorsNamespace,
                                  BSON("_id" << getCoordinatorId().toBSON()));
        return count > 0;
    }

    MigrationBlockingOperationCoordinatorV2Document getStateDocumentOnDisk() {
        ASSERT_TRUE(stateDocumentExistsOnDisk());
        DBDirectClient client(_opCtx);
        auto doc = client.findOne(NamespaceString::kShardingDDLCoordinatorsNamespace,
                                  BSON("_id" << getCoordinatorId().toBSON()));
        IDLParserContext errCtx(
            "MigrationBlockingOperationCoordinatorV2Test::getStateDocumentOnDisk()");
        return MigrationBlockingOperationCoordinatorV2Document::parse(doc, errCtx);
    }

    void assertOperationCountOnDisk(int expectedCount) {
        auto doc = getStateDocumentOnDisk();
        ASSERT_EQ(expectedCount, doc.getOperations().get_value_or({}).size());
    }

    void appendAllowMigrationsFailureResponse(Status status = Status(ErrorCodes::HostUnreachable,
                                                                     "Simulated network error")) {
        _externalState->allowMigrationsResponse.appendResponse(Fault(std::move(status)));
    }

    stdx::thread startBackgroundThread(std::function<void(OperationContext*)>&& fn) {
        return stdx::thread([=, this] {
            ThreadClient tc("backgroundTask", getServiceContext()->getService());
            auto sideOpCtx = tc->makeOperationContext();
            fn(sideOpCtx.get());
        });
    }

    std::shared_ptr<Instance> _instance;
    ServiceContext::UniqueOperationContext _opCtxHolder;
    OperationContext* _opCtx;
    std::unique_ptr<ShardingCoordinatorExternalStateFactoryForTest> _externalStateFactory;
    std::shared_ptr<ShardingCoordinatorExternalStateForTest> _externalState;
};

TEST_F(MigrationBlockingOperationCoordinatorV2Test, BeginBlocksMigrationsAndPersistsOperation) {
    auto operationId = UUID::gen();
    _instance = getInstance();

    _instance->beginOperation(_opCtx, operationId).get(_opCtx);

    ASSERT_FALSE(_externalState->migrationsAreAllowed());
    ASSERT_TRUE(stateDocumentExistsOnDisk());
    assertOperationCountOnDisk(1);
}

TEST_F(MigrationBlockingOperationCoordinatorV2Test, EndAllowsMigrationsAndCleansUp) {
    auto operationId = UUID::gen();
    _instance = getInstance();

    _instance->beginOperation(_opCtx, operationId).get(_opCtx);
    _instance->endOperation(_opCtx, operationId).get(_opCtx);

    ASSERT_OK(_instance->getCompletionFuture().getNoThrow());
    ASSERT_TRUE(_externalState->migrationsAreAllowed());
    ASSERT_FALSE(stateDocumentExistsOnDisk());
}

TEST_F(MigrationBlockingOperationCoordinatorV2Test, EndOperationDecrementsCount) {
    auto first = UUID::gen();
    auto second = UUID::gen();
    _instance = getInstance();

    _instance->beginOperation(_opCtx, first).get(_opCtx);
    _instance->beginOperation(_opCtx, second).get(_opCtx);
    assertOperationCountOnDisk(2);

    _instance->endOperation(_opCtx, first).get(_opCtx);
    assertOperationCountOnDisk(1);

    _instance->endOperation(_opCtx, second).get(_opCtx);
    ASSERT_OK(_instance->getCompletionFuture().getNoThrow());
}

TEST_F(MigrationBlockingOperationCoordinatorV2Test, BeginSameOperationMultipleTimes) {
    auto operationId = UUID::gen();
    _instance = getInstance();

    _instance->beginOperation(_opCtx, operationId).get(_opCtx);
    _instance->beginOperation(_opCtx, operationId).get(_opCtx);
    assertOperationCountOnDisk(1);
}

TEST_F(MigrationBlockingOperationCoordinatorV2Test, EndSameOperationMultipleTimes) {
    auto first = UUID::gen();
    auto second = UUID::gen();
    _instance = getInstance();

    _instance->beginOperation(_opCtx, first).get(_opCtx);
    _instance->beginOperation(_opCtx, second).get(_opCtx);

    _instance->endOperation(_opCtx, first).get(_opCtx);
    _instance->endOperation(_opCtx, first).get(_opCtx);
    assertOperationCountOnDisk(1);
}

TEST_F(MigrationBlockingOperationCoordinatorV2Test,
       BeginOperationRetriesBlockingMigrationsOnTransientError) {
    auto operationId = UUID::gen();

    // The default injected fault is retriable.
    appendAllowMigrationsFailureResponse();
    _instance = getInstance();

    ASSERT_DOES_NOT_THROW(_instance->beginOperation(_opCtx, operationId).get(_opCtx));

    ASSERT_FALSE(_externalState->migrationsAreAllowed());
    ASSERT_TRUE(stateDocumentExistsOnDisk());
    assertOperationCountOnDisk(1);
}

TEST_F(MigrationBlockingOperationCoordinatorV2Test,
       EndOperationRetriesAllowingMigrationsOnTransientError) {
    auto operationId = UUID::gen();
    _instance = getInstance();
    _instance->beginOperation(_opCtx, operationId).get(_opCtx);

    // `endOperation` resolves once the operation is durably removed, which happens before the
    // unblock runs, so the retry is observed through the completion future rather than
    // `endOperation`.
    appendAllowMigrationsFailureResponse();
    _instance->endOperation(_opCtx, operationId).get(_opCtx);

    ASSERT_OK(_instance->getCompletionFuture().getNoThrow());
    ASSERT_TRUE(_externalState->migrationsAreAllowed());
    ASSERT_FALSE(stateDocumentExistsOnDisk());
}

TEST_F(MigrationBlockingOperationCoordinatorV2Test, BlockMigrationsRetriesOnNonRetriableError) {
    auto operationId = UUID::gen();

    // The injected fault is non-retriable, but `_mustAlwaysMakeProgress` should force a retry.
    appendAllowMigrationsFailureResponse(
        Status(ErrorCodes::IllegalOperation, "Simulated non-retriable error"));
    _instance = getInstance();

    ASSERT_DOES_NOT_THROW(_instance->beginOperation(_opCtx, operationId).get(_opCtx));

    ASSERT_FALSE(_externalState->migrationsAreAllowed());
    ASSERT_TRUE(stateDocumentExistsOnDisk());
    assertOperationCountOnDisk(1);
}

TEST_F(MigrationBlockingOperationCoordinatorV2Test, UnblockMigrationsRetriesOnNonRetriableError) {
    auto operationId = UUID::gen();
    _instance = getInstance();
    _instance->beginOperation(_opCtx, operationId).get(_opCtx);

    // The injected fault is non-retriable, but `_mustAlwaysMakeProgress` should force a retry.
    appendAllowMigrationsFailureResponse(
        Status(ErrorCodes::IllegalOperation, "Simulated non-retriable error"));
    _instance->endOperation(_opCtx, operationId).get(_opCtx);

    ASSERT_OK(_instance->getCompletionFuture().getNoThrow());
    ASSERT_TRUE(_externalState->migrationsAreAllowed());
    ASSERT_FALSE(stateDocumentExistsOnDisk());
}

TEST_F(MigrationBlockingOperationCoordinatorV2Test, BeginRejectedWhileCoordinatorCleaningUp) {
    auto first = UUID::gen();
    auto second = UUID::gen();

    // The coordinator stops accepting requests before it reaches the unblock phase, where it hangs.
    auto fp = globalFailPointRegistry().find(kHangBeforeAllowingMigrations);
    auto timesEntered = fp->setMode(FailPoint::alwaysOn);

    _instance = getInstance();
    _instance->beginOperation(_opCtx, first).get(_opCtx);

    // Ending the last operation empties the set and sends the coordinator to the unblock phase.
    auto endFuture = _instance->endOperation(_opCtx, first);
    fp->waitForTimesEntered(timesEntered + 1);

    ASSERT_THROWS_CODE(_instance->beginOperation(_opCtx, second).get(_opCtx),
                       DBException,
                       ErrorCodes::MigrationBlockingOperationCoordinatorCleaningUp);

    fp->setMode(FailPoint::off);
    endFuture.get(_opCtx);
    ASSERT_OK(_instance->getCompletionFuture().getNoThrow());
    ASSERT_TRUE(_externalState->migrationsAreAllowed());
}

TEST_F(MigrationBlockingOperationCoordinatorV2Test, RecoversAndRemainsBlockedAfterFailover) {
    auto operationId = UUID::gen();
    _instance = getInstance();
    _instance->beginOperation(_opCtx, operationId).get(_opCtx);
    ASSERT_FALSE(_externalState->migrationsAreAllowed());

    stepDown();
    ASSERT_NOT_OK(_instance->getCompletionFuture().getNoThrow());
    stepUp(_opCtx);

    _instance = getExistingInstance();
    assertOperationCountOnDisk(1);
    _instance->endOperation(_opCtx, operationId).get(_opCtx);

    ASSERT_OK(_instance->getCompletionFuture().getNoThrow());
    ASSERT_TRUE(_externalState->migrationsAreAllowed());
    ASSERT_FALSE(stateDocumentExistsOnDisk());
}

TEST_F(MigrationBlockingOperationCoordinatorV2Test,
       RecoversAndUnblocksAfterStepdownWhileStoppingBlocking) {
    auto operationId = UUID::gen();
    _instance = getInstance();
    _instance->beginOperation(_opCtx, operationId).get(_opCtx);
    ASSERT_FALSE(_externalState->migrationsAreAllowed());

    // Pause in `_stopBlockingMigrations` after the phase has advanced to kStopBlockingMigrations
    // but before `allowMigrations(true)` runs.
    auto fp = globalFailPointRegistry().find(kHangBeforeAllowingMigrations);
    auto timesEntered = fp->setMode(FailPoint::alwaysOn);

    // Ending the last operation drains the queue and drives the coordinator to unblock migrations.
    // The end is acknowledged when the removal is persisted, before the unblock pause below.
    _instance->endOperation(_opCtx, operationId).get(_opCtx);
    fp->waitForTimesEntered(timesEntered + 1);
    ASSERT_FALSE(_externalState->migrationsAreAllowed());

    // Step down while paused mid-unblock, then release so `_runImpl` observes the interruption.
    stepDown();
    fp->setMode(FailPoint::off);
    ASSERT_NOT_OK(_instance->getCompletionFuture().getNoThrow());
    stepUp(_opCtx);

    // Recovery re-drives `_stopBlockingMigrations` from the persisted kStopBlockingMigrations phase
    // through to completion.
    _instance = getExistingInstance();
    ASSERT_OK(_instance->getCompletionFuture().getNoThrow());
    ASSERT_TRUE(_externalState->migrationsAreAllowed());
    ASSERT_FALSE(stateDocumentExistsOnDisk());
}

TEST_F(MigrationBlockingOperationCoordinatorV2Test, StepdownWithRunImplRunningFailsQueuedRequest) {
    // Pause inside `_startBlockingMigrations`, so `_runImpl` is running but has not yet consumed
    // the request queue.
    auto fp = globalFailPointRegistry().find(kHangBeforeBlockingMigrations);
    auto timesEntered = fp->setMode(FailPoint::alwaysOn);

    _instance = getInstance();
    fp->waitForTimesEntered(timesEntered + 1);

    // Enqueue a request that is still pending (never consumed) when we step down.
    auto beginFuture = _instance->beginOperation(_opCtx, UUID::gen());

    stepDown();
    fp->setMode(FailPoint::off);

    // The interrupt status (rather than the cleaning-up code or a broken promise) confirms the
    // queued request was failed by `_onInterrupt`'s drain.
    ASSERT_EQ(beginFuture.getNoThrow(_opCtx), ErrorCodes::InterruptedDueToReplStateChange);
    ASSERT_NOT_OK(_instance->getCompletionFuture().getNoThrow());
}

TEST_F(MigrationBlockingOperationCoordinatorV2Test, StepdownDuringConstructionFailsBeginRequest) {
    // Pause the PrimaryOnlyService before it calls run() on the instance, so construction never
    // completes. Because `beginOperation` waits for the construction completion future before
    // enqueuing, it blocks here; run it on a background thread so the test can drive the stepdown
    // that unblocks it.
    auto fp = globalFailPointRegistry().find("PrimaryOnlyServiceHangBeforeRunningInstance");
    auto timesEntered = fp->setMode(FailPoint::alwaysOn);

    _instance = getInstance();
    fp->waitForTimesEntered(timesEntered + 1);

    Status beginStatus = Status::OK();
    auto beginThread = startBackgroundThread([&](OperationContext* opCtx) {
        beginStatus = _instance->beginOperation(opCtx, UUID::gen()).getNoThrow(opCtx);
    });

    stepDown();
    fp->setMode(FailPoint::off);
    beginThread.join();

    ASSERT_EQ(beginStatus, ErrorCodes::InterruptedDueToReplStateChange);
    ASSERT_NOT_OK(_instance->getCompletionFuture().getNoThrow());
}

TEST_F(MigrationBlockingOperationCoordinatorV2Test,
       BeginSurfacesConstructionFailureInsteadOfHanging) {
    const Status kConstructionError{
        StaleDbRoutingVersion(kNamespace.dbName(), kDbVersion, boost::none, boost::none),
        "Simulated stale database version during construction"};
    _externalState->assertIsPrimaryShardForDbResponse.appendResponse(Fault(kConstructionError));

    _instance = getInstance();
    auto beginFuture = _instance->beginOperation(_opCtx, UUID::gen());

    ASSERT_EQ(_instance->getConstructionCompletionFuture().getNoThrow(),
              ErrorCodes::StaleDbVersion);
    ASSERT_TRUE(beginFuture.isReady());
    ASSERT_EQ(beginFuture.getNoThrow(_opCtx), ErrorCodes::StaleDbVersion);
    ASSERT_NOT_OK(_instance->getCompletionFuture().getNoThrow());
}

TEST_F(MigrationBlockingOperationCoordinatorV2Test, DrainsQueuedRequestsWhenOperationSetEmpties) {
    auto first = UUID::gen();
    auto second = UUID::gen();
    auto third = UUID::gen();

    // Pause before the coordinator consumes the queue so we can buffer several requests behind the
    // one that empties the operation set.
    auto fp = globalFailPointRegistry().find(kHangBeforeBlockingMigrations);
    auto timesEntered = fp->setMode(FailPoint::alwaysOn);
    _instance = getInstance();
    fp->waitForTimesEntered(timesEntered + 1);

    // begin(first) then end(first) empties the set and makes the coordinator stop accepting
    // requests; begin(second) and end(third) stay queued and must be drained.
    auto beginFirst = _instance->beginOperation(_opCtx, first);
    auto endFirst = _instance->endOperation(_opCtx, first);
    auto beginSecond = _instance->beginOperation(_opCtx, second);
    auto endThird = _instance->endOperation(_opCtx, third);

    fp->setMode(FailPoint::off);

    ASSERT_OK(beginFirst.getNoThrow(_opCtx));
    ASSERT_OK(endFirst.getNoThrow(_opCtx));
    // A queued begin is rejected with the retryable cleaning-up error, and a queued end gets the
    // same error (which its command treats as success).
    ASSERT_EQ(ErrorCodes::MigrationBlockingOperationCoordinatorCleaningUp,
              beginSecond.getNoThrow(_opCtx).code());
    ASSERT_EQ(ErrorCodes::MigrationBlockingOperationCoordinatorCleaningUp,
              endThird.getNoThrow(_opCtx).code());

    ASSERT_OK(_instance->getCompletionFuture().getNoThrow());
    ASSERT_TRUE(_externalState->migrationsAreAllowed());
    ASSERT_FALSE(stateDocumentExistsOnDisk());
}

TEST_F(MigrationBlockingOperationCoordinatorV2Test, BeginOperationRollsBackOnPersistFailure) {
    auto fp = globalFailPointRegistry().find("failToPersistMigrationBlockingOperations");
    fp->setMode(FailPoint::nTimes, 1);

    _instance = getInstance();

    // The failed persist rolls back the in-memory operation set, leaving it consistent with disk,
    // and surfaces the error to the caller.
    ASSERT_NOT_OK(_instance->beginOperation(_opCtx, UUID::gen()).getNoThrow(_opCtx));
    assertOperationCountOnDisk(0);

    ASSERT_OK(_instance->getCompletionFuture().getNoThrow());
    ASSERT_TRUE(_externalState->migrationsAreAllowed());
    ASSERT_FALSE(stateDocumentExistsOnDisk());
}

TEST_F(MigrationBlockingOperationCoordinatorV2Test, RecoversMultipleOperationsAfterFailover) {
    auto first = UUID::gen();
    auto second = UUID::gen();
    _instance = getInstance();
    _instance->beginOperation(_opCtx, first).get(_opCtx);
    _instance->beginOperation(_opCtx, second).get(_opCtx);
    assertOperationCountOnDisk(2);

    stepDown();
    ASSERT_NOT_OK(_instance->getCompletionFuture().getNoThrow());
    stepUp(_opCtx);

    _instance = getExistingInstance();
    assertOperationCountOnDisk(2);
    ASSERT_FALSE(_externalState->migrationsAreAllowed());

    _instance->endOperation(_opCtx, first).get(_opCtx);
    _instance->endOperation(_opCtx, second).get(_opCtx);

    ASSERT_OK(_instance->getCompletionFuture().getNoThrow());
    ASSERT_TRUE(_externalState->migrationsAreAllowed());
    ASSERT_FALSE(stateDocumentExistsOnDisk());
}

TEST_F(MigrationBlockingOperationCoordinatorV2Test, RecoversWhenBlockingWithNoOperationsOnDisk) {
    auto operationId = UUID::gen();
    _instance = getInstance();
    _instance->beginOperation(_opCtx, operationId).get(_opCtx);
    ASSERT_FALSE(_externalState->migrationsAreAllowed());
    assertOperationCountOnDisk(1);
    {
        DBDirectClient client(_opCtx);
        client.update(NamespaceString::kShardingDDLCoordinatorsNamespace,
                      BSON("_id" << getCoordinatorId().toBSON()),
                      BSON("$set" << BSON("operations" << BSONArray())));
    }
    ASSERT_EQ(MigrationBlockingOperationCoordinatorV2PhaseEnum::kCurrentlyBlockingMigrations,
              getStateDocumentOnDisk().getPhase());
    assertOperationCountOnDisk(0);

    stepDown();
    ASSERT_NOT_OK(_instance->getCompletionFuture().getNoThrow());
    stepUp(_opCtx);

    _instance = getExistingInstance();
    ASSERT_OK(_instance->getCompletionFuture().getNoThrow());
    ASSERT_TRUE(_externalState->migrationsAreAllowed());
    ASSERT_FALSE(stateDocumentExistsOnDisk());
}

using MigrationBlockingOperationCoordinatorV2TestDeathTest =
    MigrationBlockingOperationCoordinatorV2Test;

DEATH_TEST_F(MigrationBlockingOperationCoordinatorV2TestDeathTest,
             InvalidInitialStateDocument,
             "Operations should only exist while migrations are being blocked") {
    auto stateDocument = createStateDocument();

    std::vector<UUID> operations = {UUID::gen()};
    stateDocument.setOperations(operations);

    Instance::getOrCreate(_opCtx, _service, stateDocument.toBSON());
}

DEATH_TEST_F(MigrationBlockingOperationCoordinatorV2TestDeathTest,
             DuplicateOperationsOnDisk,
             "Duplicate operations found on disk with same UUID") {
    auto stateDocument = createStateDocument();
    stateDocument.setPhase(
        MigrationBlockingOperationCoordinatorV2PhaseEnum::kCurrentlyBlockingMigrations);

    auto duplicateUUID = UUID::gen();
    std::vector<UUID> duplicateOperationVector = {duplicateUUID, duplicateUUID};
    stateDocument.setOperations(duplicateOperationVector);

    Instance::getOrCreate(_opCtx, _service, stateDocument.toBSON());
}

}  // namespace
}  // namespace mongo
