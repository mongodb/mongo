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

#include "mongo/base/error_codes.h"
#include "mongo/bson/json.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator.h"
#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator_external_state.h"
#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator_gen.h"
#include "mongo/db/s/primary_only_service_helpers/phase_transition_progress_gen.h"
#include "mongo/db/s/primary_only_service_helpers/with_automatic_retry.h"
#include "mongo/db/session/internal_session_pool.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

const Status kRetryableError{ErrorCodes::Interrupted, "Interrupted"};
const auto kNamespace = NamespaceString::createNamespaceString_forTest("test.coll");
constexpr auto kPauseInPhaseFailpoint = "pauseDuringMultiUpdateCoordinatorPhaseTransition";
constexpr auto kPauseInPhaseFailpointAlternate =
    "pauseDuringMultiUpdateCoordinatorPhaseTransitionAlternate";
constexpr auto kRunFailpoint = "hangDuringMultiUpdateCoordinatorRun";

BSONObj updateSuccessResponseBSONObj() {
    BSONObjBuilder bodyBob;
    bodyBob.append("nModified", 2);
    bodyBob.append("n", 2);
    bodyBob.append("ok", 1);
    return bodyBob.obj();
}

BSONObj updateFailedResponseBSONObj() {
    BSONObjBuilder bodyBob;
    bodyBob.append("ok", 0);
    bodyBob.append("code", ErrorCodes::Error::UpdateOperationFailed);
    return bodyBob.obj();
}

class ExternalStateFake {
public:
    void setAutoCompleteUpdates(bool value) {
        _autoCompleteUpdates = value;
    }

    void setAutoCompleteResponse(BSONObj response) {
        _autoCompleteResponse = std::move(response);
    }

    void completeUpdates(BSONObj result) {
        ASSERT_TRUE(_isUpdatePending.load());
        OpMsgBuilder builder;
        builder.setBody(std::move(result));
        auto response = builder.finish();
        response.header().setId(nextMessageId());
        response.header().setResponseToMsgId(1);
        OpMsg::appendChecksum(&response);

        DbResponse dbResponse;
        dbResponse.response = std::move(response);
        _updateResponse.emplaceValue(std::move(dbResponse));
        _isUpdatePending.store(false);
    }

    int getStartBlockingMigrationsCount() const {
        return _startBlockingMigrationsCount.load();
    }

    int getStopBlockingMigrationsCount() const {
        return _stopBlockingMigrationsCount.load();
    }

    bool sessionIsCheckedOut() const {
        return _sessionIsCheckedOut.load();
    }

    bool migrationsAreBlocked() const {
        return _migrationsAreBlocked.load();
    }

    bool updatesArePending() const {
        return _isUpdatePending.load();
    }

private:
    Future<DbResponse> beginUpdates() {
        ASSERT_FALSE(_isUpdatePending.load());
        _isUpdatePending.store(true);

        if (_autoCompleteUpdates) {
            completeUpdates(std::move(_autoCompleteResponse));
        }

        return _updateResponse.getFuture().unsafeToInlineFuture();
    }

    void startBlockingMigrations() {
        _startBlockingMigrationsCount.fetchAndAdd(1);
        _migrationsAreBlocked.store(true);
    }

    void stopBlockingMigrations() {
        _stopBlockingMigrationsCount.fetchAndAdd(1);
        _migrationsAreBlocked.store(false);
    }

    bool isUpdatePending() const {
        // If we release the session too early, our check for pending operations may see some
        // unrelated operation if someone else checks out the same session.
        ASSERT_TRUE(_sessionIsCheckedOut.load());
        return _isUpdatePending.load();
    }

    InternalSessionPool::Session acquireSession() {
        ASSERT_FALSE(_sessionIsCheckedOut.load());
        _sessionIsCheckedOut.store(true);
        return _session;
    }

    void releaseSession(InternalSessionPool::Session session) {
        ASSERT_TRUE(_sessionIsCheckedOut.load());
        ASSERT_EQ(session.getSessionId(), _session.getSessionId());
        ASSERT_EQ(session.getTxnNumber(), _session.getTxnNumber());
        _sessionIsCheckedOut.store(false);
    }

    friend class MultiUpdateCoordinatorExternalStateForTest;
    SharedPromise<DbResponse> _updateResponse;
    bool _autoCompleteUpdates{true};
    BSONObj _autoCompleteResponse{updateSuccessResponseBSONObj()};
    AtomicWord<bool> _isUpdatePending{false};
    AtomicWord<int> _startBlockingMigrationsCount{0};
    AtomicWord<int> _stopBlockingMigrationsCount{0};
    const InternalSessionPool::Session _session{makeLogicalSessionIdForTest(), 42};
    AtomicWord<bool> _sessionIsCheckedOut{false};
    AtomicWord<bool> _migrationsAreBlocked{false};
};

class MultiUpdateCoordinatorExternalStateForTest : public MultiUpdateCoordinatorExternalState {
public:
    explicit MultiUpdateCoordinatorExternalStateForTest(
        std::shared_ptr<ExternalStateFake> fakeState)
        : _fakeState{fakeState} {}

    Future<DbResponse> sendClusterUpdateCommandToShards(OperationContext* opCtx,
                                                        const Message& message) const override {
        return _fakeState->beginUpdates();
    }

    void startBlockingMigrations(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const UUID& operationId) override {
        _fakeState->startBlockingMigrations();
    }

    void stopBlockingMigrations(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const UUID& operationId) override {
        _fakeState->stopBlockingMigrations();
    }

    int getStartBlockingMigrationsCount() const {
        return _fakeState->getStartBlockingMigrationsCount();
    }

    int getStopBlockingMigrationsCount() const {
        return _fakeState->getStopBlockingMigrationsCount();
    }

    bool isUpdatePending(OperationContext* opCtx,
                         const NamespaceString& nss,
                         mongo::AggregateCommandRequest& request) const override {
        return _fakeState->isUpdatePending();
    }

    InternalSessionPool::Session acquireSession() override {
        return _fakeState->acquireSession();
    }

    void releaseSession(InternalSessionPool::Session session) override {
        return _fakeState->releaseSession(std::move(session));
    }

private:
    std::shared_ptr<ExternalStateFake> _fakeState;
};

class MultiUpdateCoordinatorExternalStateFactoryForTest
    : public MultiUpdateCoordinatorExternalStateFactory {
public:
    MultiUpdateCoordinatorExternalStateFactoryForTest(std::shared_ptr<ExternalStateFake> fakeState)
        : _fakeState{fakeState} {}

    std::unique_ptr<MultiUpdateCoordinatorExternalState> createExternalState() const {
        return std::make_unique<MultiUpdateCoordinatorExternalStateForTest>(_fakeState);
    }

private:
    std::shared_ptr<ExternalStateFake> _fakeState;
};

class MultiUpdateCoordinatorServiceForTest : public MultiUpdateCoordinatorService {
public:
    explicit MultiUpdateCoordinatorServiceForTest(ServiceContext* serviceContext,
                                                  std::shared_ptr<ExternalStateFake> fakeState)
        : MultiUpdateCoordinatorService{
              serviceContext,
              std::make_unique<MultiUpdateCoordinatorExternalStateFactoryForTest>(
                  std::move(fakeState))} {}
};

class MultiUpdateCoordinatorTest : public repl::PrimaryOnlyServiceMongoDTest {
protected:
    using Service = MultiUpdateCoordinatorServiceForTest;
    using Instance = MultiUpdateCoordinatorInstance;
    using Phase = MultiUpdateCoordinatorPhaseEnum;
    using Progress = PhaseTransitionProgressEnum;

    ServiceContext::UniqueOperationContext _opCtxHolder;
    OperationContext* _opCtx;
    std::shared_ptr<ExternalStateFake> _externalState;

    void setUp() override {
        _externalState = std::make_shared<ExternalStateFake>();
        repl::PrimaryOnlyServiceMongoDTest::setUp();
        _opCtxHolder = makeOperationContext();
        _opCtx = _opCtxHolder.get();
    }

    void tearDown() override {
        ASSERT_FALSE(_externalState->migrationsAreBlocked());
        ASSERT_FALSE(_externalState->sessionIsCheckedOut());
        repl::PrimaryOnlyServiceMongoDTest::tearDown();
    }

    auto failCrudOpsOn(NamespaceString nss, ErrorCodes::Error code) {
        auto fp = globalFailPointRegistry().find("failCommand");
        auto count =
            fp->setMode(FailPoint::alwaysOn,
                        0,
                        fromjson(fmt::format("{{failCommands:['insert', 'update', 'delete'], "
                                             "namespace: '{}', failLocalClients: true, "
                                             "failInternalCommands: true, errorCode: {}}}",
                                             nss.toString_forTest(),
                                             code)));
        return std::tuple{fp, count};
    }

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<Service>(serviceContext, _externalState);
    }

    MultiUpdateCoordinatorMetadata createMetadata() {
        MultiUpdateCoordinatorMetadata metadata;
        metadata.setId(UUID::gen());

        const BSONObj query = BSON("member"
                                   << "abc123");
        const BSONObj update = BSON("$set" << BSON("points" << 50));
        auto rawUpdate = BSON("q" << query << "u" << update << "multi" << true);
        auto cmd = BSON("update"
                        << "coll"
                        << "updates" << BSON_ARRAY(rawUpdate));
        metadata.setUpdateCommand(cmd);
        metadata.setNss(kNamespace);
        return metadata;
    }

    MultiUpdateCoordinatorDocument createStateDocument() {
        MultiUpdateCoordinatorDocument document;
        document.setMetadata(createMetadata());
        return document;
    }

    MultiUpdateCoordinatorDocument getPhaseDocumentOnDisk(OperationContext* opCtx,
                                                          UUID instanceId) {
        ASSERT_TRUE(stateDocumentExistsOnDisk(opCtx, instanceId));
        DBDirectClient client(opCtx);
        auto doc = client.findOne(NamespaceString::kMultiUpdateCoordinatorsNamespace,
                                  BSON(MultiUpdateCoordinatorDocument::kIdFieldName << instanceId));
        IDLParserContext errCtx("MultiUpdateCoordinatorTest::getPhaseDocumentOnDisk()");
        return MultiUpdateCoordinatorDocument::parse(errCtx, doc);
    }

    bool stateDocumentExistsOnDisk(OperationContext* opCtx, UUID instanceId) {
        DBDirectClient client(opCtx);
        auto count = client.count(NamespaceString::kMultiUpdateCoordinatorsNamespace,
                                  BSON(MultiUpdateCoordinatorDocument::kIdFieldName << instanceId));
        return count > 0;
    }

    MultiUpdateCoordinatorDocument getPhaseDocumentOnDisk(
        const std::shared_ptr<Instance>& instance) {
        return getPhaseDocumentOnDisk(_opCtx, instance->getMetadata().getId());
    }

    std::shared_ptr<Instance> createInstance() {
        auto stateDocument = createStateDocument();
        return createInstanceFrom(stateDocument);
    }

    std::shared_ptr<Instance> createInstanceFrom(const MultiUpdateCoordinatorDocument& document) {
        return Instance::getOrCreate(_opCtx, _service, document.toBSON());
    }

    std::shared_ptr<Instance> getOrCreateInstance(OperationContext* opCtx, const UUID& id) {
        auto instanceId = BSON(MultiUpdateCoordinatorDocument::kIdFieldName << id);
        auto [maybeInstance, isPausedOrShutdown] = Instance::lookup(opCtx, _service, instanceId);
        if (!maybeInstance) {
            return createInstance();
        }

        return *maybeInstance;
    }

    auto pausePhaseTransition(Progress progress, Phase phase, const std::string& failpointName) {
        auto fp = globalFailPointRegistry().find(failpointName);
        auto count =
            fp->setMode(FailPoint::alwaysOn,
                        0,
                        fromjson(fmt::format("{{progress: '{}', phase: '{}'}}",
                                             PhaseTransitionProgress_serializer(progress),
                                             MultiUpdateCoordinatorPhase_serializer(phase))));
        return std::tuple{fp, count};
    }

    auto createInstanceInPhase(Progress progress, Phase phase) {
        auto [fp, count] = pausePhaseTransition(progress, phase, kPauseInPhaseFailpoint);
        auto instance = createInstance();
        fp->waitForTimesEntered(count + 1);
        return std::tuple{instance, fp};
    }

    std::shared_ptr<Instance> createInstancePendingUpdates() {
        _externalState->setAutoCompleteUpdates(false);
        auto fp = globalFailPointRegistry().find("hangAfterMultiUpdateCoordinatorSendsUpdates");
        auto count = fp->setMode(FailPoint::alwaysOn);
        auto instance = createInstance();
        fp->waitForTimesEntered(count + 1);
        fp->setMode(FailPoint::off);
        ASSERT_TRUE(_externalState->updatesArePending());
        return instance;
    }

    BSONObj getMetrics(const std::shared_ptr<Instance>& instance) {
        auto currentOp = instance->reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode::kExcludeIdle,
            MongoProcessInterface::CurrentOpSessionsMode::kExcludeIdle);
        ASSERT_TRUE(currentOp);
        return *currentOp;
    }

    Phase getPhase(const std::shared_ptr<Instance>& instance) {
        auto phaseString =
            getMetrics(instance).getObjectField("mutableFields").getStringField("phase").toString();
        IDLParserContext errCtx("MultiUpdateCoordinatorTest::getPhase()");
        return MultiUpdateCoordinatorPhase_parse(errCtx, phaseString);
    }

    void testPhaseTransitionUpdatesState(Phase phase) {
        auto [instance, fp] = createInstanceInPhase(Progress::kAfter, phase);
        ASSERT_EQ(getPhase(instance), phase);
        auto doc = getPhaseDocumentOnDisk(instance);
        ASSERT_EQ(doc.getMutableFields().getPhase(), phase);
        fp->setMode(FailPoint::off);

        auto status = instance->getCompletionFuture().getNoThrow();
        ASSERT_OK(status);
        auto expectedBSONObj = updateSuccessResponseBSONObj();
        ASSERT_BSONOBJ_EQ(status.getValue(), expectedBSONObj);
    }

    void assertStatusAndUpdateResponse(StatusWith<BSONObj> status,
                                       ErrorCodes::Error code = ErrorCodes::OK) {
        if (code == ErrorCodes::OK) {
            ASSERT_OK(status);
            ASSERT_BSONOBJ_EQ(status.getValue(), updateSuccessResponseBSONObj());
        } else {
            ASSERT_NOT_OK(status);
            ASSERT_EQ(status.getStatus().code(), code);
        }
    }

    auto createInstanceAndStepDown(Progress progress, Phase phase) {
        auto [instance, fp] = createInstanceInPhase(progress, phase);
        boost::optional<UUID> instanceId = instance->getMetadata().getId();
        ASSERT_TRUE(instanceId);
        stepDown();
        fp->setMode(FailPoint::off);
        ASSERT_NOT_OK(instance->getCompletionFuture().getNoThrow());
        return instanceId;
    }

    auto createInstanceAndSimulateFailover(Progress progress, Phase phase) {
        auto instanceId = createInstanceAndStepDown(progress, phase);

        auto fpAlternate = globalFailPointRegistry().find(kRunFailpoint);
        auto countAlternate = fpAlternate->setMode(FailPoint::alwaysOn);
        stepUp(_opCtx);

        auto newInstance = getOrCreateInstance(_opCtx, *instanceId);
        fpAlternate->waitForTimesEntered(countAlternate + 1);
        return std::tuple{newInstance, fpAlternate};
    }

    void testFailOverBeforePhaseTransition(Phase phase, ErrorCodes::Error code = ErrorCodes::OK) {
        auto [instance, fp] = createInstanceAndSimulateFailover(Progress::kBefore, phase);
        auto initialStartCount = _externalState->getStartBlockingMigrationsCount();
        auto initialStopCount = _externalState->getStopBlockingMigrationsCount();

        fp->setMode(FailPoint::off);
        auto status = instance->getCompletionFuture().getNoThrow();

        if (phase <= Phase::kPerformUpdate) {
            ASSERT_GT(_externalState->getStartBlockingMigrationsCount(), initialStartCount);
        } else if (phase <= Phase::kDone) {
            ASSERT_GT(_externalState->getStopBlockingMigrationsCount(), initialStopCount);
        }

        assertStatusAndUpdateResponse(status, code);
    }

    void testFailOverAfterPhaseTransition(Phase phase, ErrorCodes::Error code = ErrorCodes::OK) {
        auto [instance, fp] = createInstanceAndSimulateFailover(Progress::kAfter, phase);
        fp->setMode(FailPoint::off);
        assertStatusAndUpdateResponse(instance->getCompletionFuture().getNoThrow(), code);
    }

    void testPhaseTransitionUpdatesOnDiskStateWithWriteFailure(Phase phase) {
        auto [instance, beforeFp] = createInstanceInPhase(Progress::kBefore, phase);
        auto [afterFp, afterCount] =
            pausePhaseTransition(Progress::kAfter, phase, kPauseInPhaseFailpointAlternate);

        auto [failCrud, crudCount] = failCrudOpsOn(
            NamespaceString::kMultiUpdateCoordinatorsNamespace, kRetryableError.code());
        beforeFp->setMode(FailPoint::off);
        failCrud->waitForTimesEntered(crudCount + 1);
        failCrud->setMode(FailPoint::off);

        afterFp->waitForTimesEntered(afterCount + 1);
        ASSERT_EQ(getPhase(instance), phase);

        afterFp->setMode(FailPoint::off);

        auto status = instance->getCompletionFuture().getNoThrow();
        ASSERT_OK(status);
        auto expectedBSONObj = updateSuccessResponseBSONObj();
        ASSERT_BSONOBJ_EQ(status.getValue(), expectedBSONObj);
    }
};

TEST_F(MultiUpdateCoordinatorTest, GetMetadata) {
    auto stateDocument = createStateDocument();
    auto instance = createInstanceFrom(stateDocument);
    ASSERT_BSONOBJ_EQ(stateDocument.getMetadata().toBSON(), instance->getMetadata().toBSON());
}

TEST_F(MultiUpdateCoordinatorTest, CompletesSuccessfullyAndCleansUp) {
    auto instance = createInstance();
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
    ASSERT_FALSE(stateDocumentExistsOnDisk(_opCtx, instance->getMetadata().getId()));
}

/**
 *  Test that phase transitions update the on-disk state for each phase. kDone
 *  is omitted because the state document is deleted when transitioning to this
 *  phase.
 */

TEST_F(MultiUpdateCoordinatorTest, StateUpdatedDuringAcquireSession) {
    testPhaseTransitionUpdatesState(Phase::kAcquireSession);
}

TEST_F(MultiUpdateCoordinatorTest, StateUpdatedDuringBlockMigrations) {
    testPhaseTransitionUpdatesState(Phase::kBlockMigrations);
}

TEST_F(MultiUpdateCoordinatorTest, StateUpdatedDuringPerfomUpdate) {
    testPhaseTransitionUpdatesState(Phase::kPerformUpdate);
}

TEST_F(MultiUpdateCoordinatorTest, StateUpdatedDuringCleanup) {
    testPhaseTransitionUpdatesState(Phase::kCleanup);
}

/**
 *  Test that all phase transitions retry and complete successfully even if
 *  there is a failure when trying to update the state document once.
 */

TEST_F(MultiUpdateCoordinatorTest, RetryAfterTransientWriteFailureAcquireSession) {
    testPhaseTransitionUpdatesOnDiskStateWithWriteFailure(Phase::kAcquireSession);
}

TEST_F(MultiUpdateCoordinatorTest, RetryAfterTransientWriteFailureBlockMigrations) {
    testPhaseTransitionUpdatesOnDiskStateWithWriteFailure(Phase::kBlockMigrations);
}

TEST_F(MultiUpdateCoordinatorTest, RetryAfterTransientWriteFailurePerformUpdate) {
    testPhaseTransitionUpdatesOnDiskStateWithWriteFailure(Phase::kPerformUpdate);
}

TEST_F(MultiUpdateCoordinatorTest, RetryAfterTransientWriteFailureCleanup) {
    testPhaseTransitionUpdatesOnDiskStateWithWriteFailure(Phase::kCleanup);
}

TEST_F(MultiUpdateCoordinatorTest, RetryAfterTransientWriteFailureDone) {
    testPhaseTransitionUpdatesOnDiskStateWithWriteFailure(Phase::kDone);
}

/**
 *  Triggers a failover before transitioning to each phase (i.e. the phase in
 *  the state document will be the phase just prior to the argument to
 *  testFailOverBeforePhaseTransition). Verifies that after stepping up, the
 *  coordinator completes as expected without violating any of our constraints.
 *  kAcquireSession is a special case because there is no state document on disk
 *  before transitioning to this phase.
 */

TEST_F(MultiUpdateCoordinatorTest, StepDownBeforePersistStateDocument) {
    auto instanceId = createInstanceAndStepDown(Progress::kBefore, Phase::kAcquireSession);
    stepUp(_opCtx);
    ASSERT_FALSE(stateDocumentExistsOnDisk(_opCtx, *instanceId));
}

TEST_F(MultiUpdateCoordinatorTest, StepUpBeforeBlockMigrations) {
    testFailOverBeforePhaseTransition(Phase::kBlockMigrations);
}

TEST_F(MultiUpdateCoordinatorTest, StepUpBeforePerformUpdate) {
    testFailOverBeforePhaseTransition(Phase::kPerformUpdate);
}

TEST_F(MultiUpdateCoordinatorTest, StepUpBeforeCleanup) {
    testFailOverBeforePhaseTransition(Phase::kCleanup, ErrorCodes::duplicateCodeForTest(8126701));
}

TEST_F(MultiUpdateCoordinatorTest, StepUpBeforeDone) {
    testFailOverBeforePhaseTransition(Phase::kDone);
}

/**
 *  Triggers a failover immediately after transitioning to each phase. Verifies
 *  that after stepping up, the coordinator completes as expected without
 *  violating any of our constraints. kDone is a special case because the state
 *  document is deleted after transitioning to kDone.
 */

TEST_F(MultiUpdateCoordinatorTest, StepUpAfterAcquireSession) {
    testFailOverAfterPhaseTransition(Phase::kAcquireSession);
}

TEST_F(MultiUpdateCoordinatorTest, StepUpAfterBlockMigrations) {
    testFailOverAfterPhaseTransition(Phase::kBlockMigrations);
}

TEST_F(MultiUpdateCoordinatorTest, StepUpAfterPerformUpdate) {
    testFailOverAfterPhaseTransition(Phase::kPerformUpdate,
                                     ErrorCodes::duplicateCodeForTest(8126701));
}

TEST_F(MultiUpdateCoordinatorTest, StepUpAfterCleanup) {
    testFailOverAfterPhaseTransition(Phase::kCleanup);
}

TEST_F(MultiUpdateCoordinatorTest, StepUpAfterDeleteStateDocument) {
    auto instanceId = createInstanceAndStepDown(Progress::kAfter, Phase::kDone);
    stepUp(_opCtx);
    ASSERT_FALSE(stateDocumentExistsOnDisk(_opCtx, *instanceId));
}

TEST_F(MultiUpdateCoordinatorTest, FailsForUnsupportedCmd) {
    auto document = createStateDocument();
    const BSONObj query = BSON("member"
                               << "abc123");
    const BSONObj update = BSON("$set" << BSON("points" << 50));
    auto rawUpdate = BSON("q" << query << "u" << update << "multi" << true);
    auto cmd = BSON("NotARealUpdateCmd"
                    << "coll"
                    << "updates" << BSON_ARRAY(rawUpdate));
    document.getMetadata().setUpdateCommand(cmd);

    auto instance = createInstanceFrom(document);
    ASSERT_THROWS_CODE(instance->getCompletionFuture().get(_opCtx), DBException, 8126601);
}

TEST_F(MultiUpdateCoordinatorTest, CompletesSuccessfullyIfUnderlyingUpdateFails) {
    auto expected = updateFailedResponseBSONObj();
    _externalState->setAutoCompleteResponse(expected);
    auto instance = createInstance();
    auto result = instance->getCompletionFuture().getNoThrow();
    ASSERT_OK(result);
    ASSERT_BSONOBJ_EQ(result.getValue(), expected);
}

TEST_F(MultiUpdateCoordinatorTest, CoordinatorWaitsForPendingUpdates) {
    auto fp = globalFailPointRegistry().find("hangDuringMultiUpdateCoordinatorPendingUpdates");
    auto count = fp->setMode(FailPoint::alwaysOn);
    boost::optional<UUID> instanceId;
    {
        auto instance = createInstancePendingUpdates();
        instanceId = instance->getMetadata().getId();
    }
    stepDown();
    stepUp(_opCtx);
    auto instance = getOrCreateInstance(_opCtx, *instanceId);
    fp->waitForTimesEntered(count + 1);
    fp->setMode(FailPoint::off);
    _externalState->completeUpdates(updateSuccessResponseBSONObj());
    ASSERT_NOT_OK(instance->getCompletionFuture().getNoThrow());
}

}  // namespace
}  // namespace mongo
