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

#include "mongo/bson/json.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator.h"
#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator_gen.h"
#include "mongo/db/s/primary_only_service_helpers/state_transition_progress_gen.h"
#include "mongo/db/s/primary_only_service_helpers/with_automatic_retry.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

const Status kRetryableError{ErrorCodes::Interrupted, "Interrupted"};
constexpr auto kPauseInStateFailpoint = "pauseDuringMultiUpdateCoordinatorStateTransition";
constexpr auto kPauseInStateFailpointAlternate =
    "pauseDuringMultiUpdateCoordinatorStateTransitionAlternate";
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

struct MultiUpdateOpCounters {
    int startBlockingMigrationsCount = 0;
    int stopBlockingMigrationsCount = 0;
};

class MultiUpdateCoordinatorExternalStateForTest : public MultiUpdateCoordinatorExternalState {
public:
    explicit MultiUpdateCoordinatorExternalStateForTest(
        std::shared_ptr<MultiUpdateOpCounters> counters, bool shouldFail)
        : _counters{counters}, _shouldFail{shouldFail} {}

    Future<DbResponse> sendClusterUpdateCommandToShards(OperationContext* opCtx,
                                                        const Message& message) const override {
        OpMsgBuilder builder;

        if (_shouldFail) {
            builder.setBody(updateFailedResponseBSONObj());
        } else {
            builder.setBody(updateSuccessResponseBSONObj());
        }

        auto response = builder.finish();
        response.header().setId(nextMessageId());
        response.header().setResponseToMsgId(1);
        OpMsg::appendChecksum(&response);

        auto dbResponse = DbResponse();
        dbResponse.response = response;
        return Future<DbResponse>::makeReady(dbResponse);
    }

    void startBlockingMigrations() const override {
        _counters->startBlockingMigrationsCount++;
    }

    void stopBlockingMigrations() const override {
        _counters->stopBlockingMigrationsCount++;
    }

private:
    std::shared_ptr<MultiUpdateOpCounters> _counters;
    bool _shouldFail = false;
};

class MultiUpdateCoordinatorExternalStateFactoryForTest
    : public MultiUpdateCoordinatorExternalStateFactory {
public:
    MultiUpdateCoordinatorExternalStateFactoryForTest(
        std::shared_ptr<MultiUpdateOpCounters> counters, bool shouldFail)
        : _counters{counters}, _shouldFail{shouldFail} {}

    std::unique_ptr<MultiUpdateCoordinatorExternalState> createExternalState() const {
        return std::make_unique<MultiUpdateCoordinatorExternalStateForTest>(_counters, _shouldFail);
    }

private:
    std::shared_ptr<MultiUpdateOpCounters> _counters;
    bool _shouldFail;
};

class MultiUpdateCoordinatorServiceForTest : public MultiUpdateCoordinatorService {
public:
    explicit MultiUpdateCoordinatorServiceForTest(ServiceContext* serviceContext,
                                                  std::shared_ptr<MultiUpdateOpCounters> counters,
                                                  bool shouldFail = false)
        : MultiUpdateCoordinatorService{serviceContext,
                                        std::make_unique<
                                            MultiUpdateCoordinatorExternalStateFactoryForTest>(
                                            counters, shouldFail)},
          _serviceContext(serviceContext) {}

private:
    ServiceContext* _serviceContext;
};

class MultiUpdateCoordinatorTest : public repl::PrimaryOnlyServiceMongoDTest {
protected:
    using Service = MultiUpdateCoordinatorServiceForTest;
    using Instance = MultiUpdateCoordinatorInstance;
    using State = MultiUpdateCoordinatorStateEnum;
    using Progress = StateTransitionProgressEnum;

    ServiceContext::UniqueOperationContext _opCtxHolder;
    OperationContext* _opCtx;
    std::shared_ptr<MultiUpdateOpCounters> _counters;

    MultiUpdateCoordinatorTest() {
        _counters = std::make_shared<MultiUpdateOpCounters>();
    }

    void setUp() override {
        repl::PrimaryOnlyServiceMongoDTest::setUp();
        _opCtxHolder = makeOperationContext();
        _opCtx = _opCtxHolder.get();
    }

    const MultiUpdateOpCounters& getCounters() {
        return *_counters;
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
        return std::make_unique<Service>(serviceContext, _counters);
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
        metadata.setNss(NamespaceString::createNamespaceString_forTest("test.coll"));
        return metadata;
    }

    MultiUpdateCoordinatorDocument createStateDocument() {
        MultiUpdateCoordinatorDocument document;
        document.setMetadata(createMetadata());
        return document;
    }

    MultiUpdateCoordinatorDocument getStateDocumentOnDisk(OperationContext* opCtx,
                                                          UUID instanceId) {
        ASSERT_TRUE(stateDocumentExistsOnDisk(opCtx, instanceId));
        DBDirectClient client(opCtx);
        auto doc = client.findOne(NamespaceString::kMultiUpdateCoordinatorsNamespace,
                                  BSON(MultiUpdateCoordinatorDocument::kIdFieldName << instanceId));
        IDLParserContext errCtx("MultiUpdateCoordinatorTest::getStateDocumentOnDisk()");
        return MultiUpdateCoordinatorDocument::parse(errCtx, doc);
    }

    bool stateDocumentExistsOnDisk(OperationContext* opCtx, UUID instanceId) {
        DBDirectClient client(opCtx);
        auto count = client.count(NamespaceString::kMultiUpdateCoordinatorsNamespace,
                                  BSON(MultiUpdateCoordinatorDocument::kIdFieldName << instanceId));
        return count > 0;
    }

    MultiUpdateCoordinatorDocument getStateDocumentOnDisk(
        const std::shared_ptr<Instance>& instance) {
        return getStateDocumentOnDisk(_opCtx, instance->getMetadata().getId());
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

    auto pauseStateTransition(Progress progress, State state, const std::string& failpointName) {
        auto fp = globalFailPointRegistry().find(failpointName);
        auto count =
            fp->setMode(FailPoint::alwaysOn,
                        0,
                        fromjson(fmt::format("{{progress: '{}', state: '{}'}}",
                                             StateTransitionProgress_serializer(progress),
                                             MultiUpdateCoordinatorState_serializer(state))));
        return std::tuple{fp, count};
    }

    auto createInstanceInState(Progress progress, State state) {
        auto [fp, count] = pauseStateTransition(progress, state, kPauseInStateFailpoint);
        auto instance = createInstance();
        fp->waitForTimesEntered(count + 1);
        return std::tuple{instance, fp};
    }

    BSONObj getMetrics(const std::shared_ptr<Instance>& instance) {
        auto currentOp = instance->reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode::kExcludeIdle,
            MongoProcessInterface::CurrentOpSessionsMode::kExcludeIdle);
        ASSERT_TRUE(currentOp);
        return *currentOp;
    }

    State getState(const std::shared_ptr<Instance>& instance) {
        auto stateString =
            getMetrics(instance).getObjectField("mutableFields").getStringField("state").toString();
        IDLParserContext errCtx("MultiUpdateCoordinatorTest::getState()");
        return MultiUpdateCoordinatorState_parse(errCtx, stateString);
    }

    void testStateTransitionUpdatesState(State state) {
        auto [instance, fp] = createInstanceInState(Progress::kAfter, state);
        ASSERT_EQ(getState(instance), state);
        auto doc = getStateDocumentOnDisk(instance);
        ASSERT_EQ(doc.getMutableFields().getState(), state);
        fp->setMode(FailPoint::off);

        auto status = instance->getCompletionFuture().getNoThrow();
        ASSERT_OK(status);
        auto expectedBSONObj = updateSuccessResponseBSONObj();
        ASSERT_BSONOBJ_EQ(status.getValue(), expectedBSONObj);
    }

    void assertStatusAndUpdateResponse(StatusWith<BSONObj> status,
                                       bool expectFailureResponse = false) {
        if (expectFailureResponse) {
            ASSERT_NOT_OK(status);
            ASSERT_EQ(status.getStatus().code(), 8126701);
        } else {
            ASSERT_OK(status);
            ASSERT_BSONOBJ_EQ(status.getValue(), updateSuccessResponseBSONObj());
        }
    }

    auto createInstanceAndStepDown(Progress progress, State state) {
        auto [instance, fp] = createInstanceInState(progress, state);
        boost::optional<UUID> instanceId = instance->getMetadata().getId();
        ASSERT_TRUE(instanceId);
        stepDown();
        fp->setMode(FailPoint::off);
        ASSERT_NOT_OK(instance->getCompletionFuture().getNoThrow());
        return instanceId;
    }

    auto createInstanceAndSimulateFailover(Progress progress, State state) {
        auto instanceId = createInstanceAndStepDown(progress, state);

        auto fpAlternate = globalFailPointRegistry().find(kRunFailpoint);
        auto countAlternate = fpAlternate->setMode(FailPoint::alwaysOn);
        stepUp(_opCtx);

        auto newInstance = getOrCreateInstance(_opCtx, *instanceId);
        fpAlternate->waitForTimesEntered(countAlternate + 1);
        return std::tuple{newInstance, fpAlternate};
    }

    void testFailOverBeforeStateTransition(State state, bool expectFailureResponse = false) {
        auto [instance, fp] = createInstanceAndSimulateFailover(Progress::kBefore, state);
        auto initialStartBlockingMigrationsCount = getCounters().startBlockingMigrationsCount;
        auto initialStopBlockingMigrationsCount = getCounters().stopBlockingMigrationsCount;

        fp->setMode(FailPoint::off);
        auto status = instance->getCompletionFuture().getNoThrow();

        if (state <= State::kPerformUpdate) {
            ASSERT_GT(getCounters().startBlockingMigrationsCount,
                      initialStartBlockingMigrationsCount);
        } else if (state <= State::kDone) {
            ASSERT_GT(getCounters().stopBlockingMigrationsCount,
                      initialStopBlockingMigrationsCount);
        }

        assertStatusAndUpdateResponse(status, expectFailureResponse);
    }

    void testFailOverDuringStateTransition(State state, bool expectFailureResponse = false) {
        auto [instance, fp] = createInstanceAndSimulateFailover(Progress::kAfter, state);
        fp->setMode(FailPoint::off);
        assertStatusAndUpdateResponse(instance->getCompletionFuture().getNoThrow(),
                                      expectFailureResponse);
    }

    void testStateTransitionUpdatesOnDiskStateWithWriteFailure(State state) {
        auto [instance, beforeFp] = createInstanceInState(Progress::kBefore, state);
        auto [afterFp, afterCount] =
            pauseStateTransition(Progress::kAfter, state, kPauseInStateFailpointAlternate);

        auto [failCrud, crudCount] = failCrudOpsOn(
            NamespaceString::kMultiUpdateCoordinatorsNamespace, kRetryableError.code());
        beforeFp->setMode(FailPoint::off);
        failCrud->waitForTimesEntered(crudCount + 1);
        failCrud->setMode(FailPoint::off);

        afterFp->waitForTimesEntered(afterCount + 1);
        ASSERT_EQ(getState(instance), state);

        afterFp->setMode(FailPoint::off);

        auto status = instance->getCompletionFuture().getNoThrow();
        ASSERT_OK(status);
        auto expectedBSONObj = updateSuccessResponseBSONObj();
        ASSERT_BSONOBJ_EQ(status.getValue(), expectedBSONObj);
    }
};

class MultiUpdateCoordinatorExternalStateFailTest : public MultiUpdateCoordinatorTest {
public:
    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<Service>(serviceContext, _counters, true);
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

TEST_F(MultiUpdateCoordinatorTest, StateUpdatedDuringBlockMigrations) {
    testStateTransitionUpdatesState(State::kBlockMigrations);
}

TEST_F(MultiUpdateCoordinatorTest, StateUpdatedDuringPerfomUpdate) {
    testStateTransitionUpdatesState(State::kPerformUpdate);
}

TEST_F(MultiUpdateCoordinatorTest, StateUpdatedDuringCleanup) {
    testStateTransitionUpdatesState(State::kCleanup);
}

TEST_F(MultiUpdateCoordinatorTest, RetryAfterTransientWriteFailureBlockMigrations) {
    testStateTransitionUpdatesOnDiskStateWithWriteFailure(State::kBlockMigrations);
}

TEST_F(MultiUpdateCoordinatorTest, RetryAfterTransientWriteFailurePerformUpdate) {
    testStateTransitionUpdatesOnDiskStateWithWriteFailure(State::kPerformUpdate);
}

TEST_F(MultiUpdateCoordinatorTest, RetryAfterTransientWriteFailureCleanup) {
    testStateTransitionUpdatesOnDiskStateWithWriteFailure(State::kCleanup);
}

TEST_F(MultiUpdateCoordinatorTest, RetryAfterTransientWriteFailureDone) {
    testStateTransitionUpdatesOnDiskStateWithWriteFailure(State::kDone);
}

TEST_F(MultiUpdateCoordinatorTest, StepUpBeforeBlockMigrations) {
    testFailOverBeforeStateTransition(State::kBlockMigrations);
}

TEST_F(MultiUpdateCoordinatorTest, StepUpBeforeCleanup) {
    testFailOverBeforeStateTransition(State::kCleanup, true /* expectFailureResponse */);
}

TEST_F(MultiUpdateCoordinatorTest, StepUpBeforePerformUpdate) {
    testFailOverBeforeStateTransition(State::kPerformUpdate);
}

TEST_F(MultiUpdateCoordinatorTest, StepUpBeforeDone) {
    testFailOverBeforeStateTransition(State::kDone);
}

TEST_F(MultiUpdateCoordinatorTest, StepUpAfterBlockMigrations) {
    testFailOverDuringStateTransition(State::kBlockMigrations);
}

TEST_F(MultiUpdateCoordinatorTest, StepUpAfterPerformUpdate) {
    testFailOverDuringStateTransition(State::kPerformUpdate, true /* expectFailureResponse */);
}

TEST_F(MultiUpdateCoordinatorTest, StepUpAfterCleanup) {
    testFailOverDuringStateTransition(State::kCleanup);
}

TEST_F(MultiUpdateCoordinatorTest, StepUpAfterDone) {
    auto instanceId = createInstanceAndStepDown(Progress::kAfter, State::kDone);
    stepUp(_opCtx);
    ASSERT_FALSE(stateDocumentExistsOnDisk(_opCtx, *instanceId));
}

TEST_F(MultiUpdateCoordinatorTest, StepDownBeforeBlockMigrations) {
    auto instanceId = createInstanceAndStepDown(Progress::kBefore, State::kBlockMigrations);
    stepUp(_opCtx);
    ASSERT_FALSE(stateDocumentExistsOnDisk(_opCtx, *instanceId));
}

TEST_F(MultiUpdateCoordinatorTest, FailsForUnsupportedCmd) {
    MultiUpdateCoordinatorMetadata metadata;
    metadata.setId(UUID::gen());

    const BSONObj query = BSON("member"
                               << "abc123");
    const BSONObj update = BSON("$set" << BSON("points" << 50));
    auto rawUpdate = BSON("q" << query << "u" << update << "multi" << true);
    auto cmd = BSON("NotARealUpdateCmd"
                    << "coll"
                    << "updates" << BSON_ARRAY(rawUpdate));
    metadata.setUpdateCommand(cmd);
    metadata.setNss(NamespaceString::createNamespaceString_forTest("test.coll"));

    MultiUpdateCoordinatorDocument document;
    document.setMetadata(metadata);

    auto instance = createInstanceFrom(document);
    ASSERT_THROWS_CODE(instance->getCompletionFuture().get(_opCtx), DBException, 8126601);
}

TEST_F(MultiUpdateCoordinatorExternalStateFailTest, CompletesSuccessfullyIfUnderlyingUpdateFails) {
    auto instance = createInstance();
    auto status = instance->getCompletionFuture().getNoThrow();
    ASSERT_OK(status);
    auto expectedBSONObj = updateFailedResponseBSONObj();
    ASSERT_BSONOBJ_EQ(status.getValue(), expectedBSONObj);
}

}  // namespace
}  // namespace mongo
