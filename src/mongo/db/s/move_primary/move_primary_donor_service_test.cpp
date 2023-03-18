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

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/s/move_primary/move_primary_donor_service.h"
#include "mongo/db/s/move_primary/move_primary_recipient_cmds_gen.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

const auto kDatabaseName = NamespaceString{"testDb"};
constexpr auto kNewPrimaryShardName = "newPrimaryId";
const StatusWith<Shard::CommandResponse> kOkResponse =
    Shard::CommandResponse{boost::none, BSON("ok" << 1), Status::OK(), Status::OK()};
const Status kRetryableError{ErrorCodes::Interrupted, "Interrupted"};
const Status kUnrecoverableError{ErrorCodes::UnknownError, "Something bad happened"};

struct CommandDetails {
    CommandDetails(const ShardId& shardId,
                   const ReadPreferenceSetting& readPreference,
                   const std::string& databaseName,
                   const BSONObj& command,
                   Shard::RetryPolicy retryPolicy)
        : shardId{shardId},
          readPreference{readPreference},
          databaseName{databaseName},
          command{command.getOwned()},
          retryPolicy{retryPolicy} {}

    ShardId shardId;
    ReadPreferenceSetting readPreference;
    std::string databaseName;
    BSONObj command;
    Shard::RetryPolicy retryPolicy;
};

class FakeCommandRunner {
public:
    StatusWith<Shard::CommandResponse> runCommand(OperationContext* opCtx,
                                                  const ShardId& shardId,
                                                  const ReadPreferenceSetting& readPref,
                                                  const std::string& dbName,
                                                  const BSONObj& cmdObj,
                                                  Shard::RetryPolicy retryPolicy) {
        _commandHistory.emplace_back(shardId, readPref, dbName, cmdObj, retryPolicy);
        if (_nextResponses.empty()) {
            return kOkResponse;
        }
        auto response = _nextResponses.front();
        _nextResponses.pop_front();
        return response;
    }

    boost::optional<const CommandDetails&> getLastCommandDetails() {
        if (_commandHistory.empty()) {
            return boost::none;
        }
        return _commandHistory.back();
    }

    const std::list<CommandDetails>& getCommandHistory() const {
        return _commandHistory;
    }

    size_t getCommandsRunCount() const {
        return _commandHistory.size();
    }

    void addNextResponse(StatusWith<Shard::CommandResponse> response) {
        _nextResponses.push_back(std::move(response));
    }

private:
    std::list<StatusWith<Shard::CommandResponse>> _nextResponses;
    std::list<CommandDetails> _commandHistory;
};

class MovePrimaryDonorExternalStateForTest : public MovePrimaryDonorExternalState {
public:
    MovePrimaryDonorExternalStateForTest(const MovePrimaryCommonMetadata& metadata,
                                         const std::shared_ptr<FakeCommandRunner>& commandRunner)
        : MovePrimaryDonorExternalState{metadata}, _commandRunner{commandRunner} {}

protected:
    StatusWith<Shard::CommandResponse> runCommand(OperationContext* opCtx,
                                                  const ShardId& shardId,
                                                  const ReadPreferenceSetting& readPref,
                                                  const std::string& dbName,
                                                  const BSONObj& cmdObj,
                                                  Shard::RetryPolicy retryPolicy) {
        return _commandRunner->runCommand(opCtx, shardId, readPref, dbName, cmdObj, retryPolicy);
    };

private:
    std::shared_ptr<FakeCommandRunner> _commandRunner;
};

class MovePrimaryDonorServiceForTest : public MovePrimaryDonorService {
public:
    MovePrimaryDonorServiceForTest(ServiceContext* serviceContext,
                                   const std::shared_ptr<FakeCommandRunner>& commandRunner)
        : MovePrimaryDonorService{serviceContext}, _commandRunner{commandRunner} {}

protected:
    virtual MovePrimaryDonorDependencies makeDependencies(
        const MovePrimaryDonorDocument& initialDoc) override {
        return {std::make_unique<MovePrimaryDonorExternalStateForTest>(initialDoc.getMetadata(),
                                                                       _commandRunner)};
    }

private:
    std::shared_ptr<FakeCommandRunner> _commandRunner;
};

class MovePrimaryDonorServiceTest : public repl::PrimaryOnlyServiceMongoDTest {
protected:
    using DonorInstance = MovePrimaryDonor;

    MovePrimaryDonorServiceTest() : _commandRunner{std::make_shared<FakeCommandRunner>()} {}

    FakeCommandRunner& getCommandRunner() {
        return *_commandRunner;
    }

    auto getCurrentTimestamp() {
        return Timestamp{getServiceContext()->getFastClockSource()->now()};
    }

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<MovePrimaryDonorServiceForTest>(serviceContext, _commandRunner);
    }

    MovePrimaryCommonMetadata createMetadata() const {
        MovePrimaryCommonMetadata metadata;
        metadata.set_id(UUID::gen());
        metadata.setDatabaseName(kDatabaseName);
        metadata.setShardName(kNewPrimaryShardName);
        return metadata;
    }

    MovePrimaryDonorDocument createStateDocument() const {
        MovePrimaryDonorDocument doc;
        doc.setMetadata(createMetadata());
        return doc;
    }

    MovePrimaryDonorDocument getStateDocumentOnDisk(OperationContext* opCtx) {
        DBDirectClient client(opCtx);
        auto doc = client.findOne(NamespaceString::kMovePrimaryDonorNamespace, BSONObj{});
        IDLParserContext errCtx("MovePrimaryDonorServiceTest::getStateDocumentOnDisk()");
        return MovePrimaryDonorDocument::parse(errCtx, doc);
    }

    static constexpr auto kBefore = "before";
    static constexpr auto kPartial = "partial";
    static constexpr auto kAfter = "after";

    auto pauseStateTransitionImpl(const std::string& progress,
                                  MovePrimaryDonorStateEnum state,
                                  const std::string& failpointName) {
        auto fp = globalFailPointRegistry().find(failpointName);
        auto count = fp->setMode(FailPoint::alwaysOn,
                                 0,
                                 fromjson(fmt::format("{{progress: '{}', state: '{}'}}",
                                                      progress,
                                                      MovePrimaryDonorState_serializer(state))));
        return std::tuple{fp, count};
    }

    auto pauseStateTransition(const std::string& progress, MovePrimaryDonorStateEnum state) {
        return pauseStateTransitionImpl(
            progress, state, "pauseDuringMovePrimaryDonorStateEnumTransition");
    }

    auto pauseStateTransitionAlternate(const std::string& progress,
                                       MovePrimaryDonorStateEnum state) {
        return pauseStateTransitionImpl(
            progress, state, "pauseDuringMovePrimaryDonorStateEnumTransitionAlternate");
    }

    auto failCrudOpsOn(NamespaceString nss) {
        auto fp = globalFailPointRegistry().find("failCommand");
        auto count =
            fp->setMode(FailPoint::alwaysOn,
                        0,
                        fromjson(fmt::format("{{failCommands:['insert', 'update', 'delete'], "
                                             "namespace: '{}', failLocalClients: true, "
                                             "failInternalCommands: true, errorCode: {}}}",
                                             nss.toString(),
                                             ErrorCodes::Interrupted)));
        return std::tuple{fp, count};
    }

    BSONObj getMetrics(const std::shared_ptr<DonorInstance>& instance) {
        auto currentOp = instance->reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode::kExcludeIdle,
            MongoProcessInterface::CurrentOpSessionsMode::kExcludeIdle);
        ASSERT_TRUE(currentOp);
        return *currentOp;
    }

    std::shared_ptr<DonorInstance> createInstance() {
        auto opCtx = makeOperationContext();
        return createInstance(opCtx.get());
    }

    std::shared_ptr<DonorInstance> createInstance(OperationContext* opCtx) {
        auto stateDoc = createStateDocument();
        return DonorInstance::getOrCreate(opCtx, _service, stateDoc.toBSON());
    }

    std::shared_ptr<DonorInstance> getExistingInstance(OperationContext* opCtx, const UUID& id) {
        auto instanceId = BSON(MovePrimaryDonorDocument::k_idFieldName << id);
        auto instance = DonorInstance::lookup(opCtx, _service, instanceId);
        if (!instance) {
            return nullptr;
        }
        return *instance;
    }

    void makeReadyToComplete(const std::shared_ptr<DonorInstance>& instance) {
        instance->onBeganBlockingWrites(getCurrentTimestamp());
        instance->onReadyToForget();
    }

    void testStateTransitionUpdatesOnDiskState(MovePrimaryDonorStateEnum state) {
        auto opCtx = makeOperationContext();
        auto [fp, count] = pauseStateTransition(kAfter, state);
        auto instance = createInstance(opCtx.get());
        makeReadyToComplete(instance);
        fp->waitForTimesEntered(count + 1);
        auto onDiskState = getStateDocumentOnDisk(opCtx.get());
        ASSERT_EQ(onDiskState.getMutableFields().getState(), state);
        fp->setMode(FailPoint::off);
        ASSERT_OK(instance->getCompletionFuture().getNoThrow());
    }

    void testStateTransitionUpdatesOnDiskStateWithWriteFailure(MovePrimaryDonorStateEnum state) {
        auto opCtx = makeOperationContext();
        auto [beforeFp, beforeCount] = pauseStateTransition(kBefore, state);
        auto [afterFp, afterCount] = pauseStateTransitionAlternate(kAfter, state);
        auto instance = createInstance(opCtx.get());
        makeReadyToComplete(instance);
        beforeFp->waitForTimesEntered(beforeCount + 1);

        auto [failCrud, crudCount] = failCrudOpsOn(NamespaceString::kMovePrimaryDonorNamespace);
        beforeFp->setMode(FailPoint::off);
        failCrud->waitForTimesEntered(crudCount + 1);
        failCrud->setMode(FailPoint::off);

        afterFp->waitForTimesEntered(afterCount + 1);
        auto onDiskState = getStateDocumentOnDisk(opCtx.get());
        ASSERT_EQ(onDiskState.getMutableFields().getState(), state);
        afterFp->setMode(FailPoint::off);
        ASSERT_OK(instance->getCompletionFuture().getNoThrow());
    }

    void testStateTransitionUpdatesInMemoryState(MovePrimaryDonorStateEnum state) {
        auto [fp, count] = pauseStateTransition(kAfter, state);
        auto instance = createInstance();
        makeReadyToComplete(instance);
        fp->waitForTimesEntered(count + 1);

        ASSERT_EQ(getMetrics(instance).getStringField("state"),
                  MovePrimaryDonorState_serializer(state));

        fp->setMode(FailPoint::off);
        ASSERT_OK(instance->getCompletionFuture().getNoThrow());
    }

    bool mustBlockWritesToReachState(MovePrimaryDonorStateEnum state) {
        return state >= MovePrimaryDonorStateEnum::kBlockingWrites;
    }

    void testStepUpInState(MovePrimaryDonorStateEnum state) {
        auto opCtx = makeOperationContext();
        auto stateDoc = createStateDocument();

        {
            auto [fp, count] = pauseStateTransition(kAfter, state);
            auto instance = DonorInstance::getOrCreate(opCtx.get(), _service, stateDoc.toBSON());

            if (mustBlockWritesToReachState(state)) {
                instance->onBeganBlockingWrites(getCurrentTimestamp());
            }

            fp->waitForTimesEntered(count + 1);

            stepDown();
            fp->setMode(FailPoint::off);
            ASSERT_NOT_OK(instance->getCompletionFuture().getNoThrow());
        }

        auto fp = globalFailPointRegistry().find("pauseBeforeBeginningMovePrimaryDonorWorkflow");
        auto count = fp->setMode(FailPoint::alwaysOn);
        stepUp(opCtx.get());
        auto instance = getExistingInstance(opCtx.get(), stateDoc.get_id());

        if (!mustBlockWritesToReachState(state)) {
            instance->onBeganBlockingWrites(getCurrentTimestamp());
        }

        fp->waitForTimesEntered(count + 1);
        fp->setMode(FailPoint::off);
        instance->onReadyToForget();
        ASSERT_OK(instance->getCompletionFuture().getNoThrow());
    }

private:
    std::shared_ptr<FakeCommandRunner> _commandRunner;
};

TEST_F(MovePrimaryDonorServiceTest, GetMetadata) {
    auto opCtx = makeOperationContext();
    auto stateDoc = createStateDocument();
    auto instance = DonorInstance::getOrCreate(opCtx.get(), _service, stateDoc.toBSON());
    ASSERT_BSONOBJ_EQ(stateDoc.getMetadata().toBSON(), instance->getMetadata().toBSON());
}

TEST_F(MovePrimaryDonorServiceTest, CannotCreateTwoInstancesForSameDb) {
    auto opCtx = makeOperationContext();
    auto stateDoc = createStateDocument();
    auto instance = DonorInstance::getOrCreate(opCtx.get(), _service, stateDoc.toBSON());
    auto otherStateDoc = stateDoc;
    otherStateDoc.getMetadata().set_id(UUID::gen());
    ASSERT_THROWS_CODE(DonorInstance::getOrCreate(opCtx.get(), _service, otherStateDoc.toBSON()),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(MovePrimaryDonorServiceTest, SameUuidMustHaveSameDb) {
    auto opCtx = makeOperationContext();
    auto stateDoc = createStateDocument();
    auto instance = DonorInstance::getOrCreate(opCtx.get(), _service, stateDoc.toBSON());
    auto otherStateDoc = stateDoc;
    otherStateDoc.getMetadata().setDatabaseName(NamespaceString{"someOtherDb"});
    ASSERT_THROWS_CODE(DonorInstance::getOrCreate(opCtx.get(), _service, otherStateDoc.toBSON()),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(MovePrimaryDonorServiceTest, SameUuidMustHaveSameRecipient) {
    auto opCtx = makeOperationContext();
    auto stateDoc = createStateDocument();
    auto instance = DonorInstance::getOrCreate(opCtx.get(), _service, stateDoc.toBSON());
    auto otherStateDoc = stateDoc;
    otherStateDoc.getMetadata().setShardName("someOtherShard");
    ASSERT_THROWS_CODE(DonorInstance::getOrCreate(opCtx.get(), _service, otherStateDoc.toBSON()),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(MovePrimaryDonorServiceTest, StateDocumentIsPersistedAfterInitializing) {
    testStateTransitionUpdatesOnDiskState(MovePrimaryDonorStateEnum::kInitializing);
}

TEST_F(MovePrimaryDonorServiceTest, StateDocumentIsUpdatedDuringCloning) {
    testStateTransitionUpdatesOnDiskState(MovePrimaryDonorStateEnum::kCloning);
}

TEST_F(MovePrimaryDonorServiceTest, StateDocumentIsUpdatedDuringWaitingToBlockWrites) {
    testStateTransitionUpdatesOnDiskState(MovePrimaryDonorStateEnum::kWaitingToBlockWrites);
}

TEST_F(MovePrimaryDonorServiceTest, StateDocumentIsUpdatedDuringBlockingWrites) {
    testStateTransitionUpdatesOnDiskState(MovePrimaryDonorStateEnum::kBlockingWrites);
}

TEST_F(MovePrimaryDonorServiceTest, StateDocumentIsUpdatedDuringPrepared) {
    testStateTransitionUpdatesOnDiskState(MovePrimaryDonorStateEnum::kPrepared);
}

TEST_F(MovePrimaryDonorServiceTest, StateDocumentInsertionRetriesIfWriteFails) {
    testStateTransitionUpdatesOnDiskStateWithWriteFailure(MovePrimaryDonorStateEnum::kInitializing);
}

TEST_F(MovePrimaryDonorServiceTest, TransitionToCloningRetriesIfWriteFails) {
    testStateTransitionUpdatesOnDiskStateWithWriteFailure(MovePrimaryDonorStateEnum::kCloning);
}

TEST_F(MovePrimaryDonorServiceTest, TransitionToWaitingToBlockWritesRetriesIfWriteFails) {
    testStateTransitionUpdatesOnDiskStateWithWriteFailure(
        MovePrimaryDonorStateEnum::kWaitingToBlockWrites);
}

TEST_F(MovePrimaryDonorServiceTest, TransitionToBlockingWritesRetriesIfWriteFails) {
    testStateTransitionUpdatesOnDiskStateWithWriteFailure(
        MovePrimaryDonorStateEnum::kBlockingWrites);
}

TEST_F(MovePrimaryDonorServiceTest, TransitionToPreparedRetriesIfWriteFails) {
    testStateTransitionUpdatesOnDiskStateWithWriteFailure(MovePrimaryDonorStateEnum::kPrepared);
}

TEST_F(MovePrimaryDonorServiceTest, InitializingUpdatesInMemoryState) {
    testStateTransitionUpdatesInMemoryState(MovePrimaryDonorStateEnum::kInitializing);
}

TEST_F(MovePrimaryDonorServiceTest, CloningUpdatesInMemoryState) {
    testStateTransitionUpdatesInMemoryState(MovePrimaryDonorStateEnum::kCloning);
}

TEST_F(MovePrimaryDonorServiceTest, WaitingToBlockWritesUpdatesInMemoryState) {
    testStateTransitionUpdatesInMemoryState(MovePrimaryDonorStateEnum::kWaitingToBlockWrites);
}

TEST_F(MovePrimaryDonorServiceTest, BlockingWritesUpdatesInMemoryState) {
    testStateTransitionUpdatesInMemoryState(MovePrimaryDonorStateEnum::kBlockingWrites);
}

TEST_F(MovePrimaryDonorServiceTest, PreparedUpdatesInMemoryState) {
    testStateTransitionUpdatesInMemoryState(MovePrimaryDonorStateEnum::kPrepared);
}

TEST_F(MovePrimaryDonorServiceTest, StepUpInInitializing) {
    testStepUpInState(MovePrimaryDonorStateEnum::kInitializing);
}

TEST_F(MovePrimaryDonorServiceTest, StepUpInCloning) {
    testStepUpInState(MovePrimaryDonorStateEnum::kCloning);
}

TEST_F(MovePrimaryDonorServiceTest, StepUpInWaitingToBlockWrites) {
    testStepUpInState(MovePrimaryDonorStateEnum::kWaitingToBlockWrites);
}

TEST_F(MovePrimaryDonorServiceTest, StepUpInBlockingWrites) {
    testStepUpInState(MovePrimaryDonorStateEnum::kBlockingWrites);
}

TEST_F(MovePrimaryDonorServiceTest, StepUpInPrepared) {
    testStepUpInState(MovePrimaryDonorStateEnum::kPrepared);
}

TEST_F(MovePrimaryDonorServiceTest, CloningSendsSyncDataCommandWithoutTimestamp) {
    auto [fp, count] =
        pauseStateTransition(kBefore, MovePrimaryDonorStateEnum::kWaitingToBlockWrites);
    auto instance = createInstance();
    fp->waitForTimesEntered(count + 1);

    ASSERT_GT(getCommandRunner().getCommandsRunCount(), 0);
    auto details = getCommandRunner().getLastCommandDetails();
    const auto& command = details->command;
    ASSERT_TRUE(command.hasField(MovePrimaryRecipientSyncData::kCommandName));
    ASSERT_FALSE(command.hasField(
        MovePrimaryRecipientSyncData::kReturnAfterReachingDonorTimestampFieldName));

    fp->setMode(FailPoint::off);
    makeReadyToComplete(instance);
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(MovePrimaryDonorServiceTest, CloningRetriesSyncDataCommandOnFailure) {
    auto [beforeCloning, beforeCount] =
        pauseStateTransition(kAfter, MovePrimaryDonorStateEnum::kCloning);
    auto [afterCloning, afterCount] =
        pauseStateTransitionAlternate(kBefore, MovePrimaryDonorStateEnum::kWaitingToBlockWrites);
    auto instance = createInstance();
    beforeCloning->waitForTimesEntered(beforeCount + 1);

    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), 0);
    getCommandRunner().addNextResponse(kRetryableError);

    beforeCloning->setMode(FailPoint::off);
    afterCloning->waitForTimesEntered(afterCount + 1);

    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), 2);

    afterCloning->setMode(FailPoint::off);
    makeReadyToComplete(instance);
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(MovePrimaryDonorServiceTest, WaitingToBlockWritesSetsReadyToBlockWritesFuture) {
    auto instance = createInstance();
    ASSERT_OK(instance->getReadyToBlockWritesFuture().getNoThrow());
    ASSERT_EQ(getMetrics(instance).getStringField("state"),
              MovePrimaryDonorState_serializer(MovePrimaryDonorStateEnum::kWaitingToBlockWrites));
    makeReadyToComplete(instance);
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(MovePrimaryDonorServiceTest, WaitingToBlockWritesPersistsBlockTimestamp) {
    auto opCtx = makeOperationContext();
    auto instance = createInstance(opCtx.get());
    auto [fp, count] = pauseStateTransition(kBefore, MovePrimaryDonorStateEnum::kBlockingWrites);
    ASSERT_OK(instance->getReadyToBlockWritesFuture().getNoThrow());

    const Timestamp timestamp{getServiceContext()->getFastClockSource()->now()};
    instance->onBeganBlockingWrites(timestamp);
    fp->waitForTimesEntered(count + 1);

    auto docOnDisk = getStateDocumentOnDisk(opCtx.get());
    ASSERT_EQ(docOnDisk.getMutableFields().getBlockingWritesTimestamp(), timestamp);

    fp->setMode(FailPoint::off);
    instance->onReadyToForget();
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(MovePrimaryDonorServiceTest, BlockingWritesSendsSyncDataCommandWithTimestamp) {
    auto [beforeFp, beforeFpCount] =
        pauseStateTransition(kAfter, MovePrimaryDonorStateEnum::kBlockingWrites);
    auto instance = createInstance();
    const auto kTimestamp = Timestamp{1234, 2};
    instance->onBeganBlockingWrites(kTimestamp);
    beforeFp->waitForTimesEntered(beforeFpCount + 1);

    auto beforeCommandCount = getCommandRunner().getCommandsRunCount();

    auto [afterFp, afterFpCount] =
        pauseStateTransitionAlternate(kBefore, MovePrimaryDonorStateEnum::kPrepared);
    beforeFp->setMode(FailPoint::off);
    afterFp->waitForTimesEntered(afterFpCount + 1);

    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), beforeCommandCount + 1);
    auto details = getCommandRunner().getLastCommandDetails();
    const auto& command = details->command;
    ASSERT_TRUE(command.hasField(MovePrimaryRecipientSyncData::kCommandName));
    ASSERT_EQ(
        command.getField(MovePrimaryRecipientSyncData::kReturnAfterReachingDonorTimestampFieldName)
            .timestamp(),
        kTimestamp);

    afterFp->setMode(FailPoint::off);
    instance->onReadyToForget();
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(MovePrimaryDonorServiceTest, BlockingWritesSyncDataCommandRetriesOnFailure) {
    auto [beforeFp, beforeFpCount] =
        pauseStateTransition(kAfter, MovePrimaryDonorStateEnum::kBlockingWrites);
    auto instance = createInstance();
    makeReadyToComplete(instance);
    beforeFp->waitForTimesEntered(beforeFpCount + 1);

    auto beforeCommandCount = getCommandRunner().getCommandsRunCount();
    getCommandRunner().addNextResponse(kRetryableError);

    auto [afterFp, afterFpCount] =
        pauseStateTransitionAlternate(kBefore, MovePrimaryDonorStateEnum::kPrepared);
    beforeFp->setMode(FailPoint::off);
    afterFp->waitForTimesEntered(afterFpCount + 1);

    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), beforeCommandCount + 2);

    afterFp->setMode(FailPoint::off);
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(MovePrimaryDonorServiceTest,
       BlockingWritesSyncDataCommandSendsProperTimestampAfterFailover) {
    auto opCtx = makeOperationContext();
    const auto kTimestamp = Timestamp{1234, 2};
    boost::optional<UUID> instanceId;
    {
        auto [fp, count] = pauseStateTransition(kAfter, MovePrimaryDonorStateEnum::kBlockingWrites);
        auto instance = createInstance(opCtx.get());
        instanceId = instance->getMetadata().get_id();
        instance->onBeganBlockingWrites(kTimestamp);
        fp->waitForTimesEntered(count + 1);
        stepDown();
        fp->setMode(FailPoint::off);
        ASSERT_NOT_OK(instance->getCompletionFuture().getNoThrow());
    }

    auto [fp, count] = pauseStateTransition(kBefore, MovePrimaryDonorStateEnum::kPrepared);
    stepUp(opCtx.get());
    auto instance = getExistingInstance(opCtx.get(), *instanceId);
    fp->waitForTimesEntered(count + 1);

    auto details = getCommandRunner().getLastCommandDetails();
    const auto& command = details->command;
    ASSERT_TRUE(command.hasField(MovePrimaryRecipientSyncData::kCommandName));
    ASSERT_EQ(
        command.getField(MovePrimaryRecipientSyncData::kReturnAfterReachingDonorTimestampFieldName)
            .timestamp(),
        kTimestamp);

    fp->setMode(FailPoint::off);
    instance->onReadyToForget();
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(MovePrimaryDonorServiceTest, PreparedSetsDecisionFuture) {
    auto instance = createInstance();
    instance->onBeganBlockingWrites(getCurrentTimestamp());
    ASSERT_OK(instance->getDecisionFuture().getNoThrow());
    instance->onReadyToForget();
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(MovePrimaryDonorServiceTest, StepUpAfterPreparedSendsNoAdditionalCommands) {
    auto opCtx = makeOperationContext();
    boost::optional<UUID> instanceId;
    {
        auto [fp, count] = pauseStateTransition(kAfter, MovePrimaryDonorStateEnum::kPrepared);
        auto instance = createInstance(opCtx.get());
        instanceId = instance->getMetadata().get_id();
        instance->onBeganBlockingWrites(getCurrentTimestamp());
        fp->waitForTimesEntered(count + 1);
        stepDown();
        fp->setMode(FailPoint::off);
        ASSERT_NOT_OK(instance->getCompletionFuture().getNoThrow());
    }
    const auto beforeCommandCount = getCommandRunner().getCommandsRunCount();
    stepUp(opCtx.get());
    auto instance = getExistingInstance(opCtx.get(), *instanceId);
    instance->onReadyToForget();

    auto fp = globalFailPointRegistry().find("pauseBeforeBeginningMovePrimaryDonorCleanup");
    auto fpCount = fp->setMode(FailPoint::alwaysOn);
    fp->waitForTimesEntered(fpCount + 1);
    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), beforeCommandCount);
    fp->setMode(FailPoint::off);

    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(MovePrimaryDonorServiceTest, ForgetSendsForgetToRecipient) {
    auto beforeFp = globalFailPointRegistry().find("pauseBeforeBeginningMovePrimaryDonorCleanup");
    auto beforeFpCount = beforeFp->setMode(FailPoint::alwaysOn);
    auto instance = createInstance();
    makeReadyToComplete(instance);
    beforeFp->waitForTimesEntered(beforeFpCount + 1);

    auto beforeCommandCount = getCommandRunner().getCommandsRunCount();

    auto [afterFp, afterFpCount] = pauseStateTransition(kBefore, MovePrimaryDonorStateEnum::kDone);
    beforeFp->setMode(FailPoint::off);
    afterFp->waitForTimesEntered(afterFpCount + 1);

    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), beforeCommandCount + 1);
    auto details = getCommandRunner().getLastCommandDetails();
    const auto& command = details->command;
    ASSERT_TRUE(command.hasField(MovePrimaryRecipientForgetMigration::kCommandName));

    afterFp->setMode(FailPoint::off);
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(MovePrimaryDonorServiceTest, ForgetRetriesRecipientForgetOnAnyFailure) {
    auto beforeFp = globalFailPointRegistry().find("pauseBeforeBeginningMovePrimaryDonorCleanup");
    auto beforeFpCount = beforeFp->setMode(FailPoint::alwaysOn);
    auto instance = createInstance();
    makeReadyToComplete(instance);
    beforeFp->waitForTimesEntered(beforeFpCount + 1);

    auto beforeCommandCount = getCommandRunner().getCommandsRunCount();
    getCommandRunner().addNextResponse(kRetryableError);
    getCommandRunner().addNextResponse(kUnrecoverableError);

    auto [afterFp, afterFpCount] = pauseStateTransition(kBefore, MovePrimaryDonorStateEnum::kDone);
    beforeFp->setMode(FailPoint::off);
    afterFp->waitForTimesEntered(afterFpCount + 1);

    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), beforeCommandCount + 3);

    afterFp->setMode(FailPoint::off);
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(MovePrimaryDonorServiceTest, ForgetRetriesRecipientForgetAfterFailover) {
    auto opCtx = makeOperationContext();
    boost::optional<UUID> instanceId;
    {
        auto fp = globalFailPointRegistry().find("pauseBeforeBeginningMovePrimaryDonorCleanup");
        auto count = fp->setMode(FailPoint::alwaysOn);
        auto instance = createInstance(opCtx.get());
        instanceId = instance->getMetadata().get_id();
        makeReadyToComplete(instance);
        fp->waitForTimesEntered(count + 1);
        stepDown();
        fp->setMode(FailPoint::off);
        ASSERT_NOT_OK(instance->getCompletionFuture().getNoThrow());
    }

    auto beforeCommandCount = getCommandRunner().getCommandsRunCount();

    stepUp(opCtx.get());
    auto instance = getExistingInstance(opCtx.get(), *instanceId);
    instance->onReadyToForget();

    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), beforeCommandCount + 1);
}

TEST_F(MovePrimaryDonorServiceTest, StateDocumentRemovedAfterSuccess) {
    auto opCtx = makeOperationContext();
    auto instance = createInstance(opCtx.get());
    makeReadyToComplete(instance);
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());

    DBDirectClient client(opCtx.get());
    auto doc = client.findOne(NamespaceString::kMovePrimaryDonorNamespace, BSONObj{});
    ASSERT_TRUE(doc.isEmpty());
}

}  // namespace
}  // namespace mongo
