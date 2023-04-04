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
constexpr auto kOldPrimaryShardName = "oldPrimaryId";
constexpr auto kNewPrimaryShardName = "newPrimaryId";
const StatusWith<Shard::CommandResponse> kOkResponse =
    Shard::CommandResponse{boost::none, BSON("ok" << 1), Status::OK(), Status::OK()};
const Status kRetryableError{ErrorCodes::Interrupted, "Interrupted"};
const Status kUnrecoverableError{ErrorCodes::UnknownError, "Something bad happened"};
const Status kAbortedError{ErrorCodes::MovePrimaryAborted, "MovePrimary aborted"};

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
        stdx::unique_lock lock{_mutex};
        _commandHistory.emplace_back(shardId, readPref, dbName, cmdObj, retryPolicy);
        if (_nextResponses.empty()) {
            return kOkResponse;
        }
        auto response = _nextResponses.front();
        _nextResponses.pop_front();
        return response;
    }

    boost::optional<const CommandDetails&> getLastCommandDetails() {
        stdx::unique_lock lock{_mutex};
        if (_commandHistory.empty()) {
            return boost::none;
        }
        return _commandHistory.back();
    }

    const std::list<CommandDetails>& getCommandHistory() const {
        stdx::unique_lock lock{_mutex};
        return _commandHistory;
    }

    size_t getCommandsRunCount() const {
        stdx::unique_lock lock{_mutex};
        return _commandHistory.size();
    }

    void addNextResponse(StatusWith<Shard::CommandResponse> response) {
        stdx::unique_lock lock{_mutex};
        _nextResponses.push_back(std::move(response));
    }

private:
    mutable Mutex _mutex;
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
    virtual MovePrimaryDonorDependencies _makeDependencies(
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
        metadata.setMigrationId(UUID::gen());
        metadata.setDatabaseName(kDatabaseName);
        metadata.setFromShardName(kOldPrimaryShardName);
        metadata.setToShardName(kNewPrimaryShardName);
        return metadata;
    }

    MovePrimaryDonorDocument createStateDocument() const {
        MovePrimaryDonorDocument doc;
        auto metadata = createMetadata();
        doc.setMetadata(metadata);
        doc.setId(metadata.getMigrationId());
        return doc;
    }

    MovePrimaryDonorDocument getStateDocumentOnDisk(OperationContext* opCtx, UUID instanceId) {
        DBDirectClient client(opCtx);
        auto doc = client.findOne(NamespaceString::kMovePrimaryDonorNamespace,
                                  BSON(MovePrimaryDonorDocument::kIdFieldName << instanceId));
        IDLParserContext errCtx("MovePrimaryDonorServiceTest::getStateDocumentOnDisk()");
        return MovePrimaryDonorDocument::parse(errCtx, doc);
    }

    MovePrimaryDonorDocument getStateDocumentOnDisk(
        OperationContext* opCtx, const std::shared_ptr<DonorInstance>& instance) {
        return getStateDocumentOnDisk(opCtx, instance->getMetadata().getMigrationId());
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

    auto failCrudOpsOn(NamespaceString nss, ErrorCodes::Error code) {
        auto fp = globalFailPointRegistry().find("failCommand");
        auto count =
            fp->setMode(FailPoint::alwaysOn,
                        0,
                        fromjson(fmt::format("{{failCommands:['insert', 'update', 'delete'], "
                                             "namespace: '{}', failLocalClients: true, "
                                             "failInternalCommands: true, errorCode: {}}}",
                                             nss.toString(),
                                             code)));
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
        auto instanceId = BSON(MovePrimaryDonorDocument::kIdFieldName << id);
        auto instance = DonorInstance::lookup(opCtx, _service, instanceId);
        if (!instance) {
            return nullptr;
        }
        return *instance;
    }

    bool mustBlockWritesToReachState(MovePrimaryDonorStateEnum state) {
        if (state == MovePrimaryDonorStateEnum::kAborted) {
            return false;
        }
        return state >= MovePrimaryDonorStateEnum::kBlockingWrites;
    }

    bool mustForgetToReachState(MovePrimaryDonorStateEnum state) {
        return state == MovePrimaryDonorStateEnum::kDone;
    }

    bool mustAbortToReachState(MovePrimaryDonorStateEnum state) {
        return state == MovePrimaryDonorStateEnum::kAborted;
    }

    auto createInstanceBeforeOrAfterState(OperationContext* opCtx,
                                          const std::string& beforeOrAfter,
                                          MovePrimaryDonorStateEnum state) {
        auto [fp, count] = pauseStateTransition(beforeOrAfter, state);
        auto instance = createInstance(opCtx);
        if (mustBlockWritesToReachState(state)) {
            instance->onBeganBlockingWrites(getCurrentTimestamp());
        }
        if (mustForgetToReachState(state)) {
            instance->onReadyToForget();
        }
        if (mustAbortToReachState(state)) {
            instance->abort(kAbortedError);
        }
        fp->waitForTimesEntered(count + 1);
        if (beforeOrAfter == kAfter) {
            ASSERT_EQ(getState(instance), state);
        }
        return std::tuple{instance, fp};
    }

    auto createInstanceInState(OperationContext* opCtx, MovePrimaryDonorStateEnum state) {
        return createInstanceBeforeOrAfterState(opCtx, kAfter, state);
    }

    auto createInstanceInState(MovePrimaryDonorStateEnum state) {
        auto opCtx = makeOperationContext();
        return createInstanceInState(opCtx.get(), state);
    }

    auto createInstanceBeforeState(OperationContext* opCtx, MovePrimaryDonorStateEnum state) {
        return createInstanceBeforeOrAfterState(opCtx, kBefore, state);
    }

    auto createInstanceBeforeState(MovePrimaryDonorStateEnum state) {
        auto opCtx = makeOperationContext();
        return createInstanceBeforeState(opCtx.get(), state);
    }

    MovePrimaryDonorStateEnum getState(const std::shared_ptr<DonorInstance>& instance) {
        auto stateString = getMetrics(instance).getStringField("state").toString();
        IDLParserContext errCtx("MovePrimaryDonorServiceTest::getState()");
        return MovePrimaryDonorState_parse(errCtx, stateString);
    }

    Timestamp getBlockingWritesTimestamp(OperationContext* opCtx,
                                         const std::shared_ptr<DonorInstance>& instance) {
        auto doc = getStateDocumentOnDisk(opCtx, instance->getMetadata().getMigrationId());
        auto timestamp = doc.getMutableFields().getBlockingWritesTimestamp();
        ASSERT_TRUE(timestamp.has_value());
        return *timestamp;
    }

    void makeReadyToComplete(const std::shared_ptr<DonorInstance>& instance) {
        auto state = getState(instance);
        if (state <= MovePrimaryDonorStateEnum::kWaitingToBlockWrites) {
            instance->onBeganBlockingWrites(getCurrentTimestamp());
        }
        instance->onReadyToForget();
    }

    void assertCompletesAppropriately(const std::shared_ptr<DonorInstance>& instance) {
        makeReadyToComplete(instance);
        auto result = instance->getCompletionFuture().getNoThrow();
        if (instance->isAborted()) {
            ASSERT_NOT_OK(result);
        } else {
            ASSERT_OK(result);
        }
    }

    void unpauseAndAssertCompletesAppropriately(FailPoint* fp,
                                                const std::shared_ptr<DonorInstance>& instance) {
        fp->setMode(FailPoint::off);
        assertCompletesAppropriately(instance);
    }

    auto createInstanceInStateAndSimulateFailover(OperationContext* opCtx,
                                                  MovePrimaryDonorStateEnum state) {
        boost::optional<UUID> instanceId;
        {
            auto [instance, fp] = createInstanceInState(opCtx, state);
            instanceId = instance->getMetadata().getMigrationId();
            stepDown();
            fp->setMode(FailPoint::off);
            ASSERT_NOT_OK(instance->getCompletionFuture().getNoThrow());
        }
        auto fp = globalFailPointRegistry().find("pauseBeforeBeginningMovePrimaryDonorWorkflow");
        auto count = fp->setMode(FailPoint::alwaysOn);
        stepUp(opCtx);
        auto instance = getExistingInstance(opCtx, *instanceId);
        fp->waitForTimesEntered(count + 1);
        return std::tuple{instance, fp};
    }

    void testStateTransitionUpdatesOnDiskState(MovePrimaryDonorStateEnum state) {
        auto opCtx = makeOperationContext();
        auto [instance, fp] = createInstanceInState(opCtx.get(), state);

        auto onDiskState = getStateDocumentOnDisk(opCtx.get(), instance);
        ASSERT_EQ(onDiskState.getMutableFields().getState(), state);

        unpauseAndAssertCompletesAppropriately(fp, instance);
    }

    void testStateTransitionAbortsOnUnrecoverableError(MovePrimaryDonorStateEnum state) {
        auto opCtx = makeOperationContext();
        auto [instance, beforeFp] = createInstanceBeforeState(opCtx.get(), state);
        auto [failCrud, crudCount] =
            failCrudOpsOn(NamespaceString::kMovePrimaryDonorNamespace, kUnrecoverableError.code());
        auto [afterFp, afterCount] =
            pauseStateTransitionAlternate(kAfter, MovePrimaryDonorStateEnum::kAborted);

        beforeFp->setMode(FailPoint::off);
        failCrud->waitForTimesEntered(crudCount + 1);
        failCrud->setMode(FailPoint::off);
        afterFp->waitForTimesEntered(afterCount + 1);

        unpauseAndAssertCompletesAppropriately(afterFp, instance);
        ASSERT_TRUE(instance->isAborted());
    }

    void testStateTransitionUpdatesOnDiskStateWithWriteFailure(MovePrimaryDonorStateEnum state) {
        auto opCtx = makeOperationContext();
        auto [instance, beforeFp] = createInstanceBeforeState(opCtx.get(), state);
        auto [afterFp, afterCount] = pauseStateTransitionAlternate(kAfter, state);

        auto [failCrud, crudCount] =
            failCrudOpsOn(NamespaceString::kMovePrimaryDonorNamespace, kRetryableError.code());
        beforeFp->setMode(FailPoint::off);
        failCrud->waitForTimesEntered(crudCount + 1);
        failCrud->setMode(FailPoint::off);

        afterFp->waitForTimesEntered(afterCount + 1);
        auto onDiskState = getStateDocumentOnDisk(opCtx.get(), instance);
        ASSERT_EQ(onDiskState.getMutableFields().getState(), state);

        unpauseAndAssertCompletesAppropriately(afterFp, instance);
    }

    void testStateTransitionUpdatesInMemoryState(MovePrimaryDonorStateEnum state) {
        auto [instance, fp] = createInstanceInState(state);

        ASSERT_EQ(getMetrics(instance).getStringField("state"),
                  MovePrimaryDonorState_serializer(state));

        unpauseAndAssertCompletesAppropriately(fp, instance);
    }

    void testStepUpInState(MovePrimaryDonorStateEnum state) {
        auto opCtx = makeOperationContext();
        auto [instance, fp] = createInstanceInStateAndSimulateFailover(opCtx.get(), state);
        unpauseAndAssertCompletesAppropriately(fp, instance);
    }

    void assertSentRecipientAbortCommand() {
        auto history = getCommandRunner().getCommandHistory();
        auto recipientAbortsSent = 0;
        for (const auto& details : history) {
            if (details.command.hasField(MovePrimaryRecipientAbortMigration::kCommandName)) {
                recipientAbortsSent++;
            }
        }
        ASSERT_GTE(recipientAbortsSent, 1);
    }

    void testAbortInState(MovePrimaryDonorStateEnum state) {
        auto [instance, fp] = createInstanceInState(state);
        instance->abort(kAbortedError);
        unpauseAndAssertCompletesAppropriately(fp, instance);

        ASSERT_TRUE(instance->isAborted());
        assertSentRecipientAbortCommand();
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
    auto otherMigrationId = UUID::gen();
    otherStateDoc.getMetadata().setMigrationId(otherMigrationId);
    otherStateDoc.setId(otherMigrationId);
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
    otherStateDoc.getMetadata().setToShardName("someOtherShard");
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

TEST_F(MovePrimaryDonorServiceTest, StateDocumentIsUpdatedDuringAborted) {
    testStateTransitionUpdatesOnDiskState(MovePrimaryDonorStateEnum::kAborted);
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

TEST_F(MovePrimaryDonorServiceTest, TransitionToAbortedRetriesIfWriteFails) {
    testStateTransitionUpdatesOnDiskStateWithWriteFailure(MovePrimaryDonorStateEnum::kAborted);
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

TEST_F(MovePrimaryDonorServiceTest, AbortedUpdatesInMemoryState) {
    testStateTransitionUpdatesInMemoryState(MovePrimaryDonorStateEnum::kAborted);
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

TEST_F(MovePrimaryDonorServiceTest, StepUpInAborted) {
    testStepUpInState(MovePrimaryDonorStateEnum::kAborted);
}

TEST_F(MovePrimaryDonorServiceTest, AbortInInitializing) {
    testAbortInState(MovePrimaryDonorStateEnum::kInitializing);
}

TEST_F(MovePrimaryDonorServiceTest, AbortInCloning) {
    testAbortInState(MovePrimaryDonorStateEnum::kCloning);
}

TEST_F(MovePrimaryDonorServiceTest, AbortInWaitingToBlockWrites) {
    testAbortInState(MovePrimaryDonorStateEnum::kWaitingToBlockWrites);
}

TEST_F(MovePrimaryDonorServiceTest, AbortInBlockingWrites) {
    testAbortInState(MovePrimaryDonorStateEnum::kBlockingWrites);
}

TEST_F(MovePrimaryDonorServiceTest, AbortInPrepared) {
    testAbortInState(MovePrimaryDonorStateEnum::kPrepared);
}

TEST_F(MovePrimaryDonorServiceTest, FailTransitionToInitializing) {
    testStateTransitionAbortsOnUnrecoverableError(MovePrimaryDonorStateEnum::kInitializing);
}

TEST_F(MovePrimaryDonorServiceTest, FailTransitionToCloning) {
    testStateTransitionAbortsOnUnrecoverableError(MovePrimaryDonorStateEnum::kCloning);
}

TEST_F(MovePrimaryDonorServiceTest, FailTransitionToWaitingToBlockWrites) {
    testStateTransitionAbortsOnUnrecoverableError(MovePrimaryDonorStateEnum::kWaitingToBlockWrites);
}

TEST_F(MovePrimaryDonorServiceTest, FailTransitionToBlockingWrites) {
    testStateTransitionAbortsOnUnrecoverableError(MovePrimaryDonorStateEnum::kBlockingWrites);
}

TEST_F(MovePrimaryDonorServiceTest, FailTransitionToPrepared) {
    testStateTransitionAbortsOnUnrecoverableError(MovePrimaryDonorStateEnum::kPrepared);
}

TEST_F(MovePrimaryDonorServiceTest, CloningSendsSyncDataCommandWithoutTimestamp) {
    auto [instance, fp] = createInstanceInState(MovePrimaryDonorStateEnum::kWaitingToBlockWrites);

    ASSERT_GT(getCommandRunner().getCommandsRunCount(), 0);
    auto details = getCommandRunner().getLastCommandDetails();
    const auto& command = details->command;
    ASSERT_TRUE(command.hasField(MovePrimaryRecipientSyncData::kCommandName));
    ASSERT_FALSE(command.hasField(
        MovePrimaryRecipientSyncData::kReturnAfterReachingDonorTimestampFieldName));

    unpauseAndAssertCompletesAppropriately(fp, instance);
}

TEST_F(MovePrimaryDonorServiceTest, CloningRetriesSyncDataCommandOnFailure) {
    auto [instance, beforeCloning] = createInstanceInState(MovePrimaryDonorStateEnum::kCloning);
    auto [afterCloning, afterCount] =
        pauseStateTransitionAlternate(kBefore, MovePrimaryDonorStateEnum::kWaitingToBlockWrites);

    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), 0);
    getCommandRunner().addNextResponse(kRetryableError);

    beforeCloning->setMode(FailPoint::off);
    afterCloning->waitForTimesEntered(afterCount + 1);

    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), 2);

    unpauseAndAssertCompletesAppropriately(afterCloning, instance);
}

TEST_F(MovePrimaryDonorServiceTest, CloningAbortsOnSyncDataCommandUnrecoverableError) {
    auto [instance, fp] = createInstanceInState(MovePrimaryDonorStateEnum::kCloning);
    getCommandRunner().addNextResponse(kUnrecoverableError);

    unpauseAndAssertCompletesAppropriately(fp, instance);
    ASSERT_TRUE(instance->isAborted());
}

TEST_F(MovePrimaryDonorServiceTest, WaitingToBlockWritesSetsReadyToBlockWritesFuture) {
    auto instance = createInstance();
    ASSERT_OK(instance->getReadyToBlockWritesFuture().getNoThrow());
    ASSERT_EQ(getState(instance), MovePrimaryDonorStateEnum::kWaitingToBlockWrites);
    assertCompletesAppropriately(instance);
}

TEST_F(MovePrimaryDonorServiceTest, WaitingToBlockWritesPersistsBlockTimestamp) {
    auto opCtx = makeOperationContext();
    auto [instance, fp] =
        createInstanceInState(opCtx.get(), MovePrimaryDonorStateEnum::kWaitingToBlockWrites);
    fp->setMode(FailPoint::off);

    const auto timestamp = getCurrentTimestamp();
    instance->onBeganBlockingWrites(timestamp);

    ASSERT_OK(instance->getDecisionFuture().getNoThrow());

    auto docOnDisk = getStateDocumentOnDisk(opCtx.get(), instance);
    ASSERT_EQ(docOnDisk.getMutableFields().getBlockingWritesTimestamp(), timestamp);

    assertCompletesAppropriately(instance);
}

TEST_F(MovePrimaryDonorServiceTest, BlockingWritesSendsSyncDataCommandWithTimestamp) {
    auto [instance, fp] = createInstanceInState(MovePrimaryDonorStateEnum::kWaitingToBlockWrites);
    fp->setMode(FailPoint::off);
    const auto timestamp = getCurrentTimestamp();

    auto beforeCommandCount = getCommandRunner().getCommandsRunCount();
    instance->onBeganBlockingWrites(timestamp);

    ASSERT_OK(instance->getDecisionFuture().getNoThrow());

    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), beforeCommandCount + 1);
    auto details = getCommandRunner().getLastCommandDetails();
    const auto& command = details->command;
    ASSERT_TRUE(command.hasField(MovePrimaryRecipientSyncData::kCommandName));
    ASSERT_EQ(
        command.getField(MovePrimaryRecipientSyncData::kReturnAfterReachingDonorTimestampFieldName)
            .timestamp(),
        timestamp);

    assertCompletesAppropriately(instance);
}

TEST_F(MovePrimaryDonorServiceTest, BlockingWritesSyncDataCommandRetriesOnFailure) {
    auto [instance, fp] = createInstanceInState(MovePrimaryDonorStateEnum::kBlockingWrites);
    auto beforeCommandCount = getCommandRunner().getCommandsRunCount();
    getCommandRunner().addNextResponse(kRetryableError);

    fp->setMode(FailPoint::off);
    ASSERT_OK(instance->getDecisionFuture().getNoThrow());

    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), beforeCommandCount + 2);
    assertCompletesAppropriately(instance);
}

TEST_F(MovePrimaryDonorServiceTest, BlockingWritesAbortsOnSyncDataCommandUnrecoverableError) {
    auto [instance, fp] = createInstanceInState(MovePrimaryDonorStateEnum::kBlockingWrites);
    getCommandRunner().addNextResponse(kUnrecoverableError);

    unpauseAndAssertCompletesAppropriately(fp, instance);
    ASSERT_TRUE(instance->isAborted());
}

TEST_F(MovePrimaryDonorServiceTest,
       BlockingWritesSyncDataCommandSendsProperTimestampAfterFailover) {
    auto opCtx = makeOperationContext();
    auto [instance, fp] = createInstanceInStateAndSimulateFailover(
        opCtx.get(), MovePrimaryDonorStateEnum::kBlockingWrites);

    fp->setMode(FailPoint::off);
    const auto timestamp = getBlockingWritesTimestamp(opCtx.get(), instance);
    ASSERT_OK(instance->getDecisionFuture().getNoThrow());

    auto details = getCommandRunner().getLastCommandDetails();
    const auto& command = details->command;
    ASSERT_TRUE(command.hasField(MovePrimaryRecipientSyncData::kCommandName));
    ASSERT_EQ(
        command.getField(MovePrimaryRecipientSyncData::kReturnAfterReachingDonorTimestampFieldName)
            .timestamp(),
        timestamp);

    assertCompletesAppropriately(instance);
}

TEST_F(MovePrimaryDonorServiceTest, PreparedSetsDecisionFuture) {
    auto [instance, fp] = createInstanceInState(MovePrimaryDonorStateEnum::kPrepared);
    ASSERT_FALSE(instance->getDecisionFuture().isReady());
    fp->setMode(FailPoint::off);
    ASSERT_OK(instance->getDecisionFuture().getNoThrow());
    assertCompletesAppropriately(instance);
}

TEST_F(MovePrimaryDonorServiceTest, StepUpAfterPreparedSendsNoAdditionalCommands) {
    auto opCtx = makeOperationContext();
    auto [instance, fp] =
        createInstanceInStateAndSimulateFailover(opCtx.get(), MovePrimaryDonorStateEnum::kPrepared);

    const auto beforeCommandCount = getCommandRunner().getCommandsRunCount();
    fp->setMode(FailPoint::off);
    ASSERT_OK(instance->getDecisionFuture().getNoThrow());
    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), beforeCommandCount);

    assertCompletesAppropriately(instance);
}

TEST_F(MovePrimaryDonorServiceTest, ForgetSendsForgetToRecipient) {
    auto [instance, fp] = createInstanceInState(MovePrimaryDonorStateEnum::kPrepared);
    fp->setMode(FailPoint::off);

    auto beforeCommandCount = getCommandRunner().getCommandsRunCount();
    assertCompletesAppropriately(instance);

    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), beforeCommandCount + 1);
    auto details = getCommandRunner().getLastCommandDetails();
    const auto& command = details->command;
    ASSERT_TRUE(command.hasField(MovePrimaryRecipientForgetMigration::kCommandName));
}

TEST_F(MovePrimaryDonorServiceTest, ForgetRetriesRecipientForgetOnAnyFailure) {
    auto [instance, fp] = createInstanceInState(MovePrimaryDonorStateEnum::kPrepared);
    fp->setMode(FailPoint::off);

    auto beforeCommandCount = getCommandRunner().getCommandsRunCount();
    getCommandRunner().addNextResponse(kRetryableError);
    getCommandRunner().addNextResponse(kUnrecoverableError);

    assertCompletesAppropriately(instance);
    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), beforeCommandCount + 3);
}

TEST_F(MovePrimaryDonorServiceTest, ForgetRetriesRecipientForgetAfterFailover) {
    auto opCtx = makeOperationContext();
    auto [instance, fp] =
        createInstanceInStateAndSimulateFailover(opCtx.get(), MovePrimaryDonorStateEnum::kPrepared);

    auto beforeCommandCount = getCommandRunner().getCommandsRunCount();
    fp->setMode(FailPoint::off);

    assertCompletesAppropriately(instance);
    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), beforeCommandCount + 1);
}

TEST_F(MovePrimaryDonorServiceTest, StateDocumentRemovedAfterSuccess) {
    auto opCtx = makeOperationContext();
    auto [instance, fp] = createInstanceInState(opCtx.get(), MovePrimaryDonorStateEnum::kPrepared);
    unpauseAndAssertCompletesAppropriately(fp, instance);

    DBDirectClient client(opCtx.get());
    auto doc = client.findOne(NamespaceString::kMovePrimaryDonorNamespace, BSONObj{});
    ASSERT_TRUE(doc.isEmpty());
}

TEST_F(MovePrimaryDonorServiceTest, ReadyToBlockWritesPromiseReturnsErrorIfAborted) {
    auto opCtx = makeOperationContext();
    auto [instance, fp] =
        createInstanceInState(opCtx.get(), MovePrimaryDonorStateEnum::kInitializing);
    instance->abort(kAbortedError);
    fp->setMode(FailPoint::off);
    ASSERT_EQ(instance->getReadyToBlockWritesFuture().getNoThrow(), kAbortedError);
}

TEST_F(MovePrimaryDonorServiceTest, DecisionPromiseReturnsErrorIfAborted) {
    auto opCtx = makeOperationContext();
    auto instance = createInstance(opCtx.get());
    instance->abort(kAbortedError);
    ASSERT_EQ(instance->getDecisionFuture().getNoThrow(), kAbortedError);
}

TEST_F(MovePrimaryDonorServiceTest, CompletionPromiseReturnsErrorIfAborted) {
    auto opCtx = makeOperationContext();
    auto instance = createInstance(opCtx.get());
    instance->abort(kAbortedError);
    instance->onReadyToForget();
    ASSERT_EQ(instance->getCompletionFuture().getNoThrow(), kAbortedError);
}

TEST_F(MovePrimaryDonorServiceTest, StateDocumentRemovedAfterAbort) {
    auto opCtx = makeOperationContext();
    auto [instance, fp] = createInstanceInState(opCtx.get(), MovePrimaryDonorStateEnum::kAborted);
    unpauseAndAssertCompletesAppropriately(fp, instance);

    DBDirectClient client(opCtx.get());
    auto doc = client.findOne(NamespaceString::kMovePrimaryDonorNamespace, BSONObj{});
    ASSERT_TRUE(doc.isEmpty());
}

TEST_F(MovePrimaryDonorServiceTest, AbortSendsAbortToRecipient) {
    auto [instance, fp] = createInstanceInState(MovePrimaryDonorStateEnum::kAborted);
    auto beforeCommandCount = getCommandRunner().getCommandsRunCount();
    fp->setMode(FailPoint::off);

    fp = globalFailPointRegistry().find("pauseBeforeBeginningMovePrimaryDonorCleanup");
    auto count = fp->setMode(FailPoint::alwaysOn);
    instance->onReadyToForget();
    fp->waitForTimesEntered(count + 1);
    fp->setMode(FailPoint::off);

    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), beforeCommandCount + 1);
    auto details = getCommandRunner().getLastCommandDetails();
    const auto& command = details->command;
    ASSERT_TRUE(command.hasField(MovePrimaryRecipientAbortMigration::kCommandName));

    ASSERT_NOT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(MovePrimaryDonorServiceTest, AbortRetriesAbortToRecipientOnAnyError) {
    auto [instance, fp] = createInstanceInState(MovePrimaryDonorStateEnum::kAborted);
    auto beforeCommandCount = getCommandRunner().getCommandsRunCount();
    getCommandRunner().addNextResponse(kRetryableError);
    getCommandRunner().addNextResponse(kUnrecoverableError);
    fp->setMode(FailPoint::off);

    fp = globalFailPointRegistry().find("pauseBeforeBeginningMovePrimaryDonorCleanup");
    auto count = fp->setMode(FailPoint::alwaysOn);
    instance->onReadyToForget();
    fp->waitForTimesEntered(count + 1);
    fp->setMode(FailPoint::off);

    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), beforeCommandCount + 3);
    auto details = getCommandRunner().getLastCommandDetails();
    const auto& command = details->command;
    ASSERT_TRUE(command.hasField(MovePrimaryRecipientAbortMigration::kCommandName));

    ASSERT_NOT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(MovePrimaryDonorServiceTest, AbortRetriesAbortToRecipientAfterFailover) {
    auto opCtx = makeOperationContext();
    auto [instance, fp] =
        createInstanceInStateAndSimulateFailover(opCtx.get(), MovePrimaryDonorStateEnum::kAborted);
    auto beforeCommandCount = getCommandRunner().getCommandsRunCount();
    fp->setMode(FailPoint::off);

    fp = globalFailPointRegistry().find("pauseBeforeBeginningMovePrimaryDonorCleanup");
    auto count = fp->setMode(FailPoint::alwaysOn);
    instance->onReadyToForget();
    fp->waitForTimesEntered(count + 1);
    fp->setMode(FailPoint::off);

    ASSERT_EQ(getCommandRunner().getCommandsRunCount(), beforeCommandCount + 1);
    auto details = getCommandRunner().getLastCommandDetails();
    const auto& command = details->command;
    ASSERT_TRUE(command.hasField(MovePrimaryRecipientAbortMigration::kCommandName));

    ASSERT_NOT_OK(instance->getCompletionFuture().getNoThrow());
}

TEST_F(MovePrimaryDonorServiceTest, AbortPersistsReason) {
    auto opCtx = makeOperationContext();
    auto [instance, fp] = createInstanceInState(opCtx.get(), MovePrimaryDonorStateEnum::kAborted);

    auto doc = getStateDocumentOnDisk(opCtx.get(), instance);
    auto maybeReason = doc.getMutableFields().getAbortReason();
    ASSERT_TRUE(maybeReason);
    auto reason = *maybeReason;
    ASSERT_EQ(ErrorCodes::Error(reason["code"].numberInt()), kAbortedError.code());
    ASSERT_EQ(reason["errmsg"].String(), kAbortedError.reason());

    unpauseAndAssertCompletesAppropriately(fp, instance);
}

TEST_F(MovePrimaryDonorServiceTest, ExplicitAbortAfterDecisionSetOk) {
    auto opCtx = makeOperationContext();
    auto [instance, fp] = createInstanceInState(opCtx.get(), MovePrimaryDonorStateEnum::kPrepared);
    fp->setMode(FailPoint::off);

    ASSERT_OK(instance->getDecisionFuture().getNoThrow());
    instance->abort(kAbortedError);
    instance->onReadyToForget();

    ASSERT_EQ(instance->getCompletionFuture().getNoThrow(), kAbortedError);
    assertSentRecipientAbortCommand();
}

TEST_F(MovePrimaryDonorServiceTest, AbortDuringPartialStateTransitionMaintainsAbortReason) {
    auto opCtx = makeOperationContext();
    auto [fp, count] = pauseStateTransition(kPartial, MovePrimaryDonorStateEnum::kInitializing);
    auto instance = createInstance(opCtx.get());
    fp->waitForTimesEntered(count + 1);
    instance->abort(kAbortedError);
    fp->setMode(FailPoint::off);
    instance->onReadyToForget();
    ASSERT_EQ(instance->getCompletionFuture().getNoThrow(), kAbortedError);
}


}  // namespace
}  // namespace mongo
