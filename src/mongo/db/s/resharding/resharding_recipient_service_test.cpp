/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/s/resharding/resharding_change_event_o2_field_gen.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_data_replication.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_progress_gen.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/s/resharding/resharding_recipient_service_external_state.h"
#include "mongo/db/s/resharding/resharding_service_test_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

using RecipientStateTransitionController =
    resharding_service_test_helpers::StateTransitionController<RecipientStateEnum>;
using PauseDuringStateTransitions =
    resharding_service_test_helpers::PauseDuringStateTransitions<RecipientStateEnum>;
using OpObserverForTest =
    resharding_service_test_helpers::OpObserverForTest<RecipientStateEnum,
                                                       ReshardingRecipientDocument>;
const ShardId recipientShardId{"myShardId"};

class ExternalStateForTest : public ReshardingRecipientService::RecipientStateMachineExternalState {
public:
    ShardId myShardId(ServiceContext* serviceContext) const override {
        return recipientShardId;
    }

    void refreshCatalogCache(OperationContext* opCtx, const NamespaceString& nss) override {}

    ChunkManager getShardedCollectionRoutingInfo(OperationContext* opCtx,
                                                 const NamespaceString& nss) override {
        invariant(nss == _sourceNss);

        const OID epoch = OID::gen();
        std::vector<ChunkType> chunks = {ChunkType{
            _sourceUUID,
            ChunkRange{BSON(_currentShardKey << MINKEY), BSON(_currentShardKey << MAXKEY)},
            ChunkVersion({epoch, Timestamp(1, 1)}, {100, 0}),
            _someDonorId}};

        auto rt = RoutingTableHistory::makeNew(_sourceNss,
                                               _sourceUUID,
                                               BSON(_currentShardKey << 1),
                                               nullptr /* defaultCollator */,
                                               false /* unique */,
                                               std::move(epoch),
                                               Timestamp(1, 1),
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               boost::none /* chunkSizeBytes */,
                                               true /* allowMigrations */,
                                               chunks);

        return ChunkManager(_someDonorId,
                            DatabaseVersion(UUID::gen(), Timestamp(1, 1)),
                            _makeStandaloneRoutingTableHistory(std::move(rt)),
                            boost::none /* clusterTime */);
    }

    MigrationDestinationManager::CollectionOptionsAndUUID getCollectionOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        Timestamp afterClusterTime,
        StringData reason) override {
        invariant(nss == _sourceNss);
        return {BSONObj(), uuid};
    }

    MigrationDestinationManager::IndexesAndIdIndex getCollectionIndexes(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        Timestamp afterClusterTime,
        StringData reason) override {
        invariant(nss == _sourceNss);
        return {std::vector<BSONObj>{}, BSONObj()};
    }

    void withShardVersionRetry(OperationContext* opCtx,
                               const NamespaceString& nss,
                               StringData reason,
                               unique_function<void()> callback) override {
        callback();
    }

    void updateCoordinatorDocument(OperationContext* opCtx,
                                   const BSONObj& query,
                                   const BSONObj& update) override {}

    void clearFilteringMetadata(OperationContext* opCtx,
                                const NamespaceString& sourceNss,
                                const NamespaceString& tempReshardingNss) override {}

private:
    RoutingTableHistoryValueHandle _makeStandaloneRoutingTableHistory(RoutingTableHistory rt) {
        const auto version = rt.getVersion();
        return RoutingTableHistoryValueHandle(
            std::make_shared<RoutingTableHistory>(std::move(rt)),
            ComparableChunkVersion::makeComparableChunkVersion(version));
    }

    const StringData _currentShardKey = "oldKey";

    const NamespaceString _sourceNss{"sourcedb", "sourcecollection"};
    const UUID _sourceUUID = UUID::gen();

    const ShardId _someDonorId{"myDonorId"};
};

class RecipientOpObserverForTest : public OpObserverForTest {
public:
    RecipientOpObserverForTest(std::shared_ptr<RecipientStateTransitionController> controller)
        : OpObserverForTest(std::move(controller),
                            NamespaceString::kRecipientReshardingOperationsNamespace) {}

    RecipientStateEnum getState(const ReshardingRecipientDocument& recipientDoc) override {
        return recipientDoc.getMutableState().getState();
    }
};

class DataReplicationForTest : public ReshardingDataReplicationInterface {
public:
    SemiFuture<void> runUntilStrictlyConsistent(
        std::shared_ptr<executor::TaskExecutor> executor,
        std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
        CancellationToken cancelToken,
        CancelableOperationContextFactory opCtxFactory,
        const mongo::Date_t& startConfigTxnCloneTime) override {
        return makeReadyFutureWith([] {}).semi();
    };

    void startOplogApplication() override{};

    SharedSemiFuture<void> awaitCloningDone() override {
        return makeReadyFutureWith([] {}).share();
    };

    SharedSemiFuture<void> awaitStrictlyConsistent() override {
        return makeReadyFutureWith([] {}).share();
    };

    void shutdown() override {}

    void join() override {}
};

class ReshardingRecipientServiceForTest : public ReshardingRecipientService {
public:
    explicit ReshardingRecipientServiceForTest(ServiceContext* serviceContext)
        : ReshardingRecipientService(serviceContext), _serviceContext(serviceContext) {}

    std::shared_ptr<repl::PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) override {
        return std::make_shared<RecipientStateMachine>(
            this,
            ReshardingRecipientDocument::parse({"ReshardingRecipientServiceForTest"}, initialState),
            std::make_unique<ExternalStateForTest>(),
            [](auto...) { return std::make_unique<DataReplicationForTest>(); },
            _serviceContext);
    }

private:
    ServiceContext* _serviceContext;
};

/**
 * Tests the behavior of the ReshardingRecipientService upon recovery from failover.
 */
class ReshardingRecipientServiceTest : public repl::PrimaryOnlyServiceMongoDTest {
public:
    using RecipientStateMachine = ReshardingRecipientService::RecipientStateMachine;

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<ReshardingRecipientServiceForTest>(serviceContext);
    }

    void setUp() override {
        repl::PrimaryOnlyServiceMongoDTest::setUp();

        auto serviceContext = getServiceContext();
        auto storageMock = std::make_unique<repl::StorageInterfaceMock>();
        repl::DropPendingCollectionReaper::set(
            serviceContext, std::make_unique<repl::DropPendingCollectionReaper>(storageMock.get()));
        repl::StorageInterface::set(serviceContext, std::move(storageMock));

        _controller = std::make_shared<RecipientStateTransitionController>();
        _opObserverRegistry->addObserver(std::make_unique<RecipientOpObserverForTest>(_controller));
    }

    RecipientStateTransitionController* controller() {
        return _controller.get();
    }

    BSONObj newShardKeyPattern() {
        return BSON("newKey" << 1);
    }

    ReshardingRecipientDocument makeStateDocument(bool isAlsoDonor) {
        RecipientShardContext recipientCtx;
        recipientCtx.setState(RecipientStateEnum::kAwaitingFetchTimestamp);

        ReshardingRecipientDocument doc(std::move(recipientCtx),
                                        {ShardId{"donor1"},
                                         isAlsoDonor ? recipientShardId : ShardId{"donor2"},
                                         ShardId{"donor3"}},
                                        durationCount<Milliseconds>(Milliseconds{5}));

        NamespaceString sourceNss("sourcedb", "sourcecollection");
        auto sourceUUID = UUID::gen();
        auto commonMetadata = CommonReshardingMetadata(
            UUID::gen(),
            sourceNss,
            sourceUUID,
            resharding::constructTemporaryReshardingNss(sourceNss.db(), sourceUUID),
            newShardKeyPattern());
        commonMetadata.setStartTime(getServiceContext()->getFastClockSource()->now());

        doc.setCommonReshardingMetadata(std::move(commonMetadata));
        return doc;
    }

    void createSourceCollection(OperationContext* opCtx,
                                const ReshardingRecipientDocument& recipientDoc) {
        CollectionOptions options;
        options.uuid = recipientDoc.getSourceUUID();
        resharding::data_copy::ensureCollectionDropped(opCtx, recipientDoc.getSourceNss());
        resharding::data_copy::ensureCollectionExists(opCtx, recipientDoc.getSourceNss(), options);
    }

    void createTempReshardingCollection(OperationContext* opCtx,
                                        const ReshardingRecipientDocument& recipientDoc) {
        CollectionOptions options;
        options.uuid = recipientDoc.getReshardingUUID();
        resharding::data_copy::ensureCollectionDropped(opCtx, recipientDoc.getTempReshardingNss());
        resharding::data_copy::ensureCollectionExists(
            opCtx, recipientDoc.getTempReshardingNss(), options);
    }

    void notifyToStartCloning(OperationContext* opCtx,
                              RecipientStateMachine& recipient,
                              const ReshardingRecipientDocument& recipientDoc) {
        _onReshardingFieldsChanges(opCtx, recipient, recipientDoc, CoordinatorStateEnum::kCloning);
    }

    void notifyReshardingCommitting(OperationContext* opCtx,
                                    RecipientStateMachine& recipient,
                                    const ReshardingRecipientDocument& recipientDoc) {
        _onReshardingFieldsChanges(
            opCtx, recipient, recipientDoc, CoordinatorStateEnum::kCommitting);
    }

    void checkStateDocumentRemoved(OperationContext* opCtx) {
        AutoGetCollection recipientColl(
            opCtx, NamespaceString::kRecipientReshardingOperationsNamespace, MODE_IS);
        ASSERT_TRUE(bool(recipientColl));
        ASSERT_TRUE(bool(recipientColl->isEmpty(opCtx)));
    }

private:
    TypeCollectionRecipientFields _makeRecipientFields(
        const ReshardingRecipientDocument& recipientDoc) {
        TypeCollectionRecipientFields recipientFields{
            recipientDoc.getDonorShards(),
            recipientDoc.getSourceUUID(),
            recipientDoc.getSourceNss(),
            recipientDoc.getMinimumOperationDurationMillis()};

        auto donorShards = recipientFields.getDonorShards();
        for (unsigned i = 0; i < donorShards.size(); ++i) {
            auto minFetchTimestamp = Timestamp{10 + i, i};
            donorShards[i].setMinFetchTimestamp(minFetchTimestamp);
            recipientFields.setCloneTimestamp(minFetchTimestamp);
        }
        recipientFields.setDonorShards(std::move(donorShards));

        ReshardingApproxCopySize approxCopySize;
        approxCopySize.setApproxBytesToCopy(10000);
        approxCopySize.setApproxDocumentsToCopy(100);
        recipientFields.setReshardingApproxCopySizeStruct(std::move(approxCopySize));

        return recipientFields;
    }

    void _onReshardingFieldsChanges(OperationContext* opCtx,
                                    RecipientStateMachine& recipient,
                                    const ReshardingRecipientDocument& recipientDoc,
                                    CoordinatorStateEnum coordinatorState) {
        auto reshardingFields = TypeCollectionReshardingFields{recipientDoc.getReshardingUUID()};
        reshardingFields.setRecipientFields(_makeRecipientFields(recipientDoc));
        reshardingFields.setState(coordinatorState);
        recipient.onReshardingFieldsChanges(opCtx, reshardingFields);
    }

    std::shared_ptr<RecipientStateTransitionController> _controller;
};

TEST_F(ReshardingRecipientServiceTest, CanTransitionThroughEachStateToCompletion) {
    for (bool isAlsoDonor : {false, true}) {
        LOGV2(5551105,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "isAlsoDonor"_attr = isAlsoDonor);
        auto doc = makeStateDocument(isAlsoDonor);
        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);

        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkStateDocumentRemoved(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, StepDownStepUpEachTransition) {
    const std::vector<RecipientStateEnum> recipientStates{RecipientStateEnum::kCreatingCollection,
                                                          RecipientStateEnum::kCloning,
                                                          RecipientStateEnum::kApplying,
                                                          RecipientStateEnum::kStrictConsistency,
                                                          RecipientStateEnum::kDone};
    for (bool isAlsoDonor : {false, true}) {
        LOGV2(5551106,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "isAlsoDonor"_attr = isAlsoDonor);

        PauseDuringStateTransitions stateTransitionsGuard{controller(), recipientStates};
        auto doc = makeStateDocument(isAlsoDonor);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());
        auto opCtx = makeOperationContext();
        auto prevState = RecipientStateEnum::kUnused;

        for (const auto state : recipientStates) {

            auto recipient = [&] {
                if (prevState == RecipientStateEnum::kUnused) {
                    if (isAlsoDonor) {
                        createSourceCollection(opCtx.get(), doc);
                    }

                    RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
                    return RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());
                } else {
                    auto maybeRecipient =
                        RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
                    ASSERT_TRUE(bool(maybeRecipient));

                    // Allow the transition to prevState to succeed on this primary-only service
                    // instance.
                    stateTransitionsGuard.unset(prevState);
                    return *maybeRecipient;
                }
            }();

            if (prevState != RecipientStateEnum::kUnused) {
                // Allow the transition to prevState to succeed on this primary-only service
                // instance.
                stateTransitionsGuard.unset(prevState);
            }

            // Signal the coordinator's earliest state that allows the recipient's transition
            // into 'state' to be valid. This mimics the real system where, upon step up, the
            // new RecipientStateMachine instance gets refreshed with the coordinator's most
            // recent state.
            switch (state) {
                case RecipientStateEnum::kCreatingCollection:
                case RecipientStateEnum::kCloning: {
                    notifyToStartCloning(opCtx.get(), *recipient, doc);
                    break;
                }
                case RecipientStateEnum::kDone: {
                    notifyReshardingCommitting(opCtx.get(), *recipient, doc);
                    break;
                }
                default:
                    break;
            }

            // Step down before the transition to state can complete.
            stateTransitionsGuard.wait(state);
            stepDown();

            ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(),
                      ErrorCodes::InterruptedDueToReplStateChange);

            prevState = state;

            recipient.reset();
            stepUp(opCtx.get());
        }

        // Finally complete the operation and ensure its success.
        auto maybeRecipient = RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(bool(maybeRecipient));

        auto recipient = *maybeRecipient;

        stateTransitionsGuard.unset(RecipientStateEnum::kDone);
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkStateDocumentRemoved(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, OpCtxKilledWhileRestoringMetrics) {
    for (bool isAlsoDonor : {false, true}) {
        LOGV2(5992701,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "isAlsoDonor"_attr = isAlsoDonor);

        // Initialize recipient.
        auto doc = makeStateDocument(isAlsoDonor);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());
        auto opCtx = makeOperationContext();
        if (isAlsoDonor) {
            createSourceCollection(opCtx.get(), doc);
        }
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        // In order to restore metrics, metrics need to exist in the first place, so put the
        // recipient in the cloning state, then step down.
        PauseDuringStateTransitions stateTransitionsGuard{controller(),
                                                          RecipientStateEnum::kCloning};
        notifyToStartCloning(opCtx.get(), *recipient, doc);
        stateTransitionsGuard.wait(RecipientStateEnum::kCloning);
        stepDown();
        stateTransitionsGuard.unset(RecipientStateEnum::kCloning);
        recipient.reset();

        // Enable failpoint and step up.
        auto fp = globalFailPointRegistry().find("reshardingOpCtxKilledWhileRestoringMetrics");
        fp->setMode(FailPoint::nTimes, 1);
        stepUp(opCtx.get());

        // After the failpoint is disabled, the operation should succeed.
        auto maybeRecipient = RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(bool(maybeRecipient));
        recipient = *maybeRecipient;
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkStateDocumentRemoved(opCtx.get());
    }
}

DEATH_TEST_REGEX_F(ReshardingRecipientServiceTest, CommitFn, "4457001.*tripwire") {
    auto doc = makeStateDocument(false /* isAlsoDonor */);
    auto opCtx = makeOperationContext();
    RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
    auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

    ASSERT_THROWS_CODE(recipient->commit(), DBException, ErrorCodes::ReshardCollectionInProgress);

    notifyToStartCloning(opCtx.get(), *recipient, doc);
    recipient->awaitInStrictConsistencyOrError().get();
    recipient->commit();

    ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
}

TEST_F(ReshardingRecipientServiceTest, DropsTemporaryReshardingCollectionOnAbort) {
    for (bool isAlsoDonor : {false, true}) {
        LOGV2(5551107,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "isAlsoDonor"_attr = isAlsoDonor);

        boost::optional<PauseDuringStateTransitions> doneTransitionGuard;
        doneTransitionGuard.emplace(controller(), RecipientStateEnum::kDone);

        auto doc = makeStateDocument(isAlsoDonor);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();

        if (isAlsoDonor) {
            // If the recipient is also a donor, the original collection should already exist on
            // this shard.
            createSourceCollection(opCtx.get(), doc);
        }

        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);
        recipient->abort(false);

        doneTransitionGuard->wait(RecipientStateEnum::kDone);
        stepDown();

        ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(),
                  ErrorCodes::InterruptedDueToReplStateChange);

        recipient.reset();
        stepUp(opCtx.get());

        auto maybeRecipient = RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(bool(maybeRecipient));
        recipient = *maybeRecipient;

        doneTransitionGuard.reset();
        recipient->abort(false);

        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkStateDocumentRemoved(opCtx.get());

        if (isAlsoDonor) {
            // Verify original collection still exists after aborting.
            AutoGetCollection coll(opCtx.get(), doc.getSourceNss(), MODE_IS);
            ASSERT_TRUE(bool(coll));
            ASSERT_EQ(coll->uuid(), doc.getSourceUUID());
        }

        // Verify the temporary collection no longer exists.
        {
            AutoGetCollection coll(opCtx.get(), doc.getTempReshardingNss(), MODE_IS);
            ASSERT_FALSE(bool(coll));
        }
    }
}

TEST_F(ReshardingRecipientServiceTest, RenamesTemporaryReshardingCollectionWhenDone) {
    // The temporary collection is renamed by the donor service when the shard is also a donor. Only
    // on non-donor shards will the recipient service rename the temporary collection.
    bool isAlsoDonor = false;
    boost::optional<PauseDuringStateTransitions> stateTransitionsGuard;
    stateTransitionsGuard.emplace(controller(), RecipientStateEnum::kApplying);

    auto doc = makeStateDocument(isAlsoDonor);
    auto opCtx = makeOperationContext();
    RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
    auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

    notifyToStartCloning(opCtx.get(), *recipient, doc);

    // Wait to check the temporary collection has been created.
    stateTransitionsGuard->wait(RecipientStateEnum::kApplying);
    {
        // Check the temporary collection exists but is not yet renamed.
        AutoGetCollection coll(opCtx.get(), doc.getTempReshardingNss(), MODE_IS);
        ASSERT_TRUE(bool(coll));
        ASSERT_EQ(coll->uuid(), doc.getReshardingUUID());
    }
    stateTransitionsGuard.reset();

    notifyReshardingCommitting(opCtx.get(), *recipient, doc);

    ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
    checkStateDocumentRemoved(opCtx.get());

    {
        // Ensure the temporary collection was renamed.
        AutoGetCollection coll(opCtx.get(), doc.getSourceNss(), MODE_IS);
        ASSERT_TRUE(bool(coll));
        ASSERT_EQ(coll->uuid(), doc.getReshardingUUID());
    }
}

TEST_F(ReshardingRecipientServiceTest, WritesNoopOplogEntryOnReshardDoneCatchUp) {
    boost::optional<PauseDuringStateTransitions> doneTransitionGuard;
    doneTransitionGuard.emplace(controller(), RecipientStateEnum::kDone);

    auto doc = makeStateDocument(false /* isAlsoDonor */);
    auto opCtx = makeOperationContext();
    auto rawOpCtx = opCtx.get();
    RecipientStateMachine::insertStateDocument(rawOpCtx, doc);
    auto recipient = RecipientStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());

    notifyToStartCloning(rawOpCtx, *recipient, doc);
    notifyReshardingCommitting(opCtx.get(), *recipient, doc);

    doneTransitionGuard->wait(RecipientStateEnum::kDone);

    stepDown();
    doneTransitionGuard.reset();
    ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(),
              ErrorCodes::InterruptedDueToReplStateChange);

    DBDirectClient client(opCtx.get());
    NamespaceString sourceNss =
        resharding::constructTemporaryReshardingNss("sourcedb", doc.getSourceUUID());

    FindCommandRequest findRequest{NamespaceString::kRsOplogNamespace};
    findRequest.setFilter(
        BSON("ns" << sourceNss.toString() << "o2.reshardDoneCatchUp" << BSON("$exists" << true)));
    auto cursor = client.find(std::move(findRequest));

    ASSERT_TRUE(cursor->more()) << "Found no oplog entries for source collection";
    repl::OplogEntry op(cursor->next());
    ASSERT_FALSE(cursor->more()) << "Found multiple oplog entries for source collection: "
                                 << op.getEntry() << " and " << cursor->nextSafe();

    ReshardDoneCatchUpChangeEventO2Field expectedChangeEvent{sourceNss, doc.getReshardingUUID()};
    auto receivedChangeEvent = ReshardDoneCatchUpChangeEventO2Field::parse(
        IDLParserContext("ReshardDoneCatchUpChangeEventO2Field"), *op.getObject2());

    ASSERT_EQ(OpType_serializer(op.getOpType()), OpType_serializer(repl::OpTypeEnum::kNoop))
        << op.getEntry();
    ASSERT_EQ(*op.getUuid(), doc.getReshardingUUID()) << op.getEntry();
    ASSERT_EQ(op.getObject()["msg"].type(), BSONType::String) << op.getEntry();
    ASSERT_TRUE(receivedChangeEvent == expectedChangeEvent);
    ASSERT_TRUE(op.getFromMigrate());
    ASSERT_FALSE(bool(op.getDestinedRecipient())) << op.getEntry();
}

TEST_F(ReshardingRecipientServiceTest, WritesNoopOplogEntryForImplicitShardCollection) {
    boost::optional<PauseDuringStateTransitions> doneTransitionGuard;
    doneTransitionGuard.emplace(controller(), RecipientStateEnum::kDone);

    auto doc = makeStateDocument(false /* isAlsoDonor */);
    auto opCtx = makeOperationContext();
    auto rawOpCtx = opCtx.get();
    RecipientStateMachine::insertStateDocument(rawOpCtx, doc);
    auto recipient = RecipientStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());

    notifyToStartCloning(rawOpCtx, *recipient, doc);
    notifyReshardingCommitting(opCtx.get(), *recipient, doc);

    doneTransitionGuard->wait(RecipientStateEnum::kDone);

    stepDown();
    doneTransitionGuard.reset();
    ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(),
              ErrorCodes::InterruptedDueToReplStateChange);

    DBDirectClient client(opCtx.get());
    NamespaceString sourceNss =
        resharding::constructTemporaryReshardingNss("sourcedb", doc.getSourceUUID());

    FindCommandRequest findRequest{NamespaceString::kRsOplogNamespace};
    findRequest.setFilter(
        BSON("ns" << sourceNss.toString() << "o2.shardCollection" << BSON("$exists" << true)));
    auto cursor = client.find(std::move(findRequest));

    ASSERT_TRUE(cursor->more()) << "Found no oplog entries for source collection";
    repl::OplogEntry shardCollectionOp(cursor->next());

    ASSERT_EQ(OpType_serializer(shardCollectionOp.getOpType()),
              OpType_serializer(repl::OpTypeEnum::kNoop))
        << shardCollectionOp.getEntry();
    ASSERT_EQ(*shardCollectionOp.getUuid(), doc.getReshardingUUID())
        << shardCollectionOp.getEntry();
    ASSERT_EQ(shardCollectionOp.getObject()["msg"].type(), BSONType::Object)
        << shardCollectionOp.getEntry();
    ASSERT_FALSE(shardCollectionOp.getFromMigrate());

    auto shardCollEventExpected =
        BSON("shardCollection" << sourceNss.toString() << "shardKey" << newShardKeyPattern());
    ASSERT_BSONOBJ_EQ(*shardCollectionOp.getObject2(), shardCollEventExpected);
}

TEST_F(ReshardingRecipientServiceTest, TruncatesXLErrorOnRecipientDocument) {
    for (bool isAlsoDonor : {false, true}) {
        LOGV2(5568600,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "isAlsoDonor"_attr = isAlsoDonor);

        std::string xlErrMsg(6000, 'x');
        FailPointEnableBlock failpoint("reshardingRecipientFailsAfterTransitionToCloning",
                                       BSON("errmsg" << xlErrMsg));

        auto doc = makeStateDocument(isAlsoDonor);
        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);

        auto localTransitionToErrorFuture = recipient->awaitInStrictConsistencyOrError();
        ASSERT_OK(localTransitionToErrorFuture.getNoThrow());

        // The recipient still waits for the abort decision from the coordinator despite it having
        // errored locally. It is therefore safe to check its local state document until
        // RecipientStateMachine::abort() is called.
        {
            boost::optional<ReshardingRecipientDocument> persistedRecipientDocument;
            PersistentTaskStore<ReshardingRecipientDocument> store(
                NamespaceString::kRecipientReshardingOperationsNamespace);
            store.forEach(opCtx.get(),
                          BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName
                               << doc.getReshardingUUID()),
                          [&](const auto& recipientDocument) {
                              persistedRecipientDocument.emplace(recipientDocument);
                              return false;
                          });

            ASSERT(persistedRecipientDocument);
            auto persistedAbortReasonBSON =
                persistedRecipientDocument->getMutableState().getAbortReason();
            ASSERT(persistedAbortReasonBSON);
            // The actual abortReason will be slightly larger than kReshardErrorMaxBytes bytes due
            // to the primitive truncation algorithm - Check that the total size is less than
            // kReshardErrorMaxBytes + a couple additional bytes to provide a buffer for the field
            // name sizes.
            int maxReshardErrorBytesCeiling = resharding::kReshardErrorMaxBytes + 200;
            ASSERT_LT(persistedAbortReasonBSON->objsize(), maxReshardErrorBytesCeiling);
            ASSERT_EQ(persistedAbortReasonBSON->getIntField("code"),
                      ErrorCodes::ReshardCollectionTruncatedError);
        }

        recipient->abort(false);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
    }
}

TEST_F(ReshardingRecipientServiceTest, MetricsSuccessfullyShutDownOnUserCancelation) {
    auto doc = makeStateDocument(false);
    auto opCtx = makeOperationContext();
    RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
    auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

    notifyToStartCloning(opCtx.get(), *recipient, doc);

    auto localTransitionToErrorFuture = recipient->awaitInStrictConsistencyOrError();
    ASSERT_OK(localTransitionToErrorFuture.getNoThrow());

    recipient->abort(true);
    ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
}

TEST_F(ReshardingRecipientServiceTest, RestoreMetricsAfterStepUp) {
    const std::vector<RecipientStateEnum> recipientStates{RecipientStateEnum::kCreatingCollection,
                                                          RecipientStateEnum::kCloning,
                                                          RecipientStateEnum::kApplying,
                                                          RecipientStateEnum::kStrictConsistency,
                                                          RecipientStateEnum::kDone};
    PauseDuringStateTransitions stateTransitionsGuard{controller(), recipientStates};
    auto doc = makeStateDocument(false);
    auto instanceId =
        BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());
    auto opCtx = makeOperationContext();
    auto prevState = RecipientStateEnum::kUnused;

    auto reshardedDoc = BSON("_id" << 0 << "x" << 2 << "y" << 10);
    int64_t oplogEntriesAppliedOnEachDonor = 10;

    for (const auto state : recipientStates) {
        auto recipient = [&] {
            if (prevState == RecipientStateEnum::kUnused) {
                RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
                return RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());
            } else {
                auto maybeRecipient =
                    RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
                ASSERT_TRUE(bool(maybeRecipient));

                // Allow the transition to prevState to succeed on this primary-only service
                // instance.
                stateTransitionsGuard.unset(prevState);
                return *maybeRecipient;
            }
        }();

        if (prevState == RecipientStateEnum::kCloning) {
            std::vector<InsertStatement> inserts{InsertStatement(reshardedDoc)};
            resharding::data_copy::insertBatch(opCtx.get(), doc.getTempReshardingNss(), inserts);
        } else if (prevState == RecipientStateEnum::kApplying) {
            auto insertFn = [&](const NamespaceString nss, const InsertStatement insertStatement) {
                resharding::data_copy::ensureCollectionExists(
                    opCtx.get(), nss, CollectionOptions());

                std::vector<InsertStatement> inserts{insertStatement};
                resharding::data_copy::insertBatch(opCtx.get(), nss, inserts);
            };

            auto donorShards = doc.getDonorShards();
            unsigned int i = 0;
            for (const auto& donor : donorShards) {
                // Setup oplogBuffer collection.
                ReshardingDonorOplogId donorOplogId{{20, i}, {19, 0}};
                insertFn(resharding::getLocalOplogBufferNamespace(doc.getSourceUUID(),
                                                                  donor.getShardId()),
                         InsertStatement{BSON("_id" << donorOplogId.toBSON())});
                ++i;

                // Setup reshardingApplierProgress collection.
                ReshardingOplogApplierProgress progressDoc(
                    {doc.getReshardingUUID(), donor.getShardId()},
                    donorOplogId,
                    oplogEntriesAppliedOnEachDonor);
                insertFn(NamespaceString::kReshardingApplierProgressNamespace,
                         InsertStatement{progressDoc.toBSON()});
            }
        }

        if (prevState != RecipientStateEnum::kUnused) {
            // Allow the transition to prevState to succeed on this primary-only service
            // instance.
            stateTransitionsGuard.unset(prevState);
        }

        // Signal the coordinator's earliest state that allows the recipient's transition
        // into 'state' to be valid. This mimics the real system where, upon step up, the
        // new RecipientStateMachine instance gets refreshed with the coordinator's most
        // recent state.
        switch (state) {
            case RecipientStateEnum::kCreatingCollection:
            case RecipientStateEnum::kCloning: {
                notifyToStartCloning(opCtx.get(), *recipient, doc);
                break;
            }
            case RecipientStateEnum::kDone: {
                notifyReshardingCommitting(opCtx.get(), *recipient, doc);
                break;
            }
            default:
                break;
        }
        // Step down before the transition to state can complete.
        stateTransitionsGuard.wait(state);
        if (state == RecipientStateEnum::kStrictConsistency) {
            auto currOp = recipient
                              ->reportForCurrentOp(
                                  MongoProcessInterface::CurrentOpConnectionsMode::kExcludeIdle,
                                  MongoProcessInterface::CurrentOpSessionsMode::kExcludeIdle)
                              .get();

            ASSERT_EQ(currOp.getField("documentsCopied").numberLong(), 1L);
            ASSERT_EQ(currOp.getField("bytesCopied").numberLong(), (long)reshardedDoc.objsize());
            ASSERT_EQ(currOp.getStringField("recipientState"),
                      RecipientState_serializer(RecipientStateEnum::kApplying));
        } else if (state == RecipientStateEnum::kDone) {
            auto currOp = recipient
                              ->reportForCurrentOp(
                                  MongoProcessInterface::CurrentOpConnectionsMode::kExcludeIdle,
                                  MongoProcessInterface::CurrentOpSessionsMode::kExcludeIdle)
                              .get();

            ASSERT_EQ(currOp.getField("documentsCopied").numberLong(), 1L);
            ASSERT_EQ(currOp.getField("bytesCopied").numberLong(), (long)reshardedDoc.objsize());
            ASSERT_EQ(currOp.getField("oplogEntriesFetched").numberLong(),
                      (long)(1 * doc.getDonorShards().size()));
            ASSERT_EQ(currOp.getField("oplogEntriesApplied").numberLong(),
                      oplogEntriesAppliedOnEachDonor * doc.getDonorShards().size());
            ASSERT_EQ(currOp.getStringField("recipientState"),
                      RecipientState_serializer(RecipientStateEnum::kStrictConsistency));
        }
        stepDown();

        ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(),
                  ErrorCodes::InterruptedDueToReplStateChange);

        prevState = state;
        if (state == RecipientStateEnum::kApplying ||
            state == RecipientStateEnum::kStrictConsistency) {
            // If metrics are being verified in the next pass, ensure a retry does not alter values.
            auto fp = globalFailPointRegistry().find("reshardingOpCtxKilledWhileRestoringMetrics");
            fp->setMode(FailPoint::nTimes, 1);
        }

        recipient.reset();
        if (state != RecipientStateEnum::kDone)
            stepUp(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, RestoreMetricsAfterStepUpWithMissingProgressDoc) {
    auto doc = makeStateDocument(false);
    auto instanceId =
        BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());
    auto opCtx = makeOperationContext();

    auto donorShards = doc.getDonorShards();
    auto insertFn = [&](const NamespaceString nss, const InsertStatement insertStatement) {
        resharding::data_copy::ensureCollectionExists(opCtx.get(), nss, CollectionOptions());

        std::vector<InsertStatement> inserts{insertStatement};
        resharding::data_copy::insertBatch(opCtx.get(), nss, inserts);
    };

    for (unsigned i = 0; i < donorShards.size(); i++) {
        if (i == 0) {
            continue;
        }

        const auto& donor = donorShards[i];

        // Setup oplogBuffer collection.
        ReshardingDonorOplogId donorOplogId{{20, i}, {19, 0}};
        insertFn(resharding::getLocalOplogBufferNamespace(doc.getSourceUUID(), donor.getShardId()),
                 InsertStatement{BSON("_id" << donorOplogId.toBSON())});

        // Setup reshardingApplierProgress collection.
        ReshardingOplogApplierProgress progressDoc(
            {doc.getReshardingUUID(), donor.getShardId()}, donorOplogId, 10 /* numOplogApplied */);
        insertFn(NamespaceString::kReshardingApplierProgressNamespace,
                 InsertStatement{progressDoc.toBSON()});
    }

    auto mutableState = doc.getMutableState();
    mutableState.setState(RecipientStateEnum::kApplying);
    doc.setMutableState(mutableState);
    doc.setCloneTimestamp(Timestamp{10, 0});
    doc.setStartConfigTxnCloneTime(Date_t::now());

    auto metadata = doc.getCommonReshardingMetadata();
    metadata.setStartTime(Date_t::now());
    doc.setCommonReshardingMetadata(metadata);

    createTempReshardingCollection(opCtx.get(), doc);

    RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
    auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());
    notifyReshardingCommitting(opCtx.get(), *recipient, doc);
    ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
}

}  // namespace
}  // namespace mongo
