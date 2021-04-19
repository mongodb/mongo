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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <boost/optional/optional_io.hpp>

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_donor_service.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class OpObserverForTest;
class PauseDuringStateTransitions;

class DonorStateTransitionController {
public:
    DonorStateTransitionController() = default;

    void waitUntilStateIsReached(DonorStateEnum state) {
        stdx::unique_lock lk(_mutex);
        _waitUntilUnpausedCond.wait(lk, [this, state] { return _state == state; });
    }

private:
    friend OpObserverForTest;
    friend PauseDuringStateTransitions;

    void setPauseDuringTransition(DonorStateEnum state) {
        stdx::lock_guard lk(_mutex);
        _pauseDuringTransition.insert(state);
    }

    void unsetPauseDuringTransition(DonorStateEnum state) {
        stdx::lock_guard lk(_mutex);
        _pauseDuringTransition.erase(state);
        _pauseDuringTransitionCond.notify_all();
    }

    void notifyNewStateAndWaitUntilUnpaused(DonorStateEnum newState) {
        stdx::unique_lock lk(_mutex);
        _state = newState;
        _waitUntilUnpausedCond.notify_all();
        _pauseDuringTransitionCond.wait(
            lk, [this, newState] { return _pauseDuringTransition.count(newState) == 0; });
    }

    Mutex _mutex = MONGO_MAKE_LATCH("DonorStateTransitionController::_mutex");
    stdx::condition_variable _pauseDuringTransitionCond;
    stdx::condition_variable _waitUntilUnpausedCond;

    std::set<DonorStateEnum> _pauseDuringTransition;
    DonorStateEnum _state = DonorStateEnum::kUnused;
};

class PauseDuringStateTransitions {
public:
    PauseDuringStateTransitions(DonorStateTransitionController* controller,
                                std::vector<DonorStateEnum> states)
        : _controller{controller}, _states{std::move(states)} {
        for (auto state : _states) {
            _controller->setPauseDuringTransition(state);
        }
    }

    ~PauseDuringStateTransitions() {
        for (auto state : _states) {
            _controller->unsetPauseDuringTransition(state);
        }
    }

    void wait(DonorStateEnum state) {
        _controller->waitUntilStateIsReached(state);
    }

    void unset(DonorStateEnum state) {
        _controller->unsetPauseDuringTransition(state);
    }

private:
    DonorStateTransitionController* const _controller;
    const std::vector<DonorStateEnum> _states;
};

class OpObserverForTest : public OpObserverNoop {
public:
    OpObserverForTest(std::shared_ptr<DonorStateTransitionController> controller)
        : _controller{std::move(controller)} {}

    void onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) override {
        if (args.nss != NamespaceString::kDonorReshardingOperationsNamespace) {
            return;
        }

        auto doc =
            ReshardingDonorDocument::parse({"OpObserverForTest"}, args.updateArgs.updatedDoc);

        _controller->notifyNewStateAndWaitUntilUnpaused(doc.getMutableState().getState());
    }

private:
    std::shared_ptr<DonorStateTransitionController> _controller;
};

class ExternalStateForTest : public ReshardingDonorService::DonorStateMachineExternalState {
public:
    ShardId myShardId(ServiceContext* serviceContext) const override {
        return ShardId{"myShardId"};
    }

    void refreshCatalogCache(OperationContext* opCtx, const NamespaceString& nss) override {}

    void waitForCollectionFlush(OperationContext* opCtx, const NamespaceString& nss) override {}

    void updateCoordinatorDocument(OperationContext* opCtx,
                                   const BSONObj& query,
                                   const BSONObj& update) override {}
};

class ReshardingDonorServiceForTest : public ReshardingDonorService {
public:
    explicit ReshardingDonorServiceForTest(ServiceContext* serviceContext)
        : ReshardingDonorService(serviceContext) {}

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialState) override {
        return std::make_shared<DonorStateMachine>(std::move(initialState),
                                                   std::make_unique<ExternalStateForTest>());
    }
};

class ReshardingDonorServiceTest : public repl::PrimaryOnlyServiceMongoDTest {
public:
    using DonorStateMachine = ReshardingDonorService::DonorStateMachine;

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<ReshardingDonorServiceForTest>(serviceContext);
    }

    void setUp() override {
        repl::PrimaryOnlyServiceMongoDTest::setUp();

        auto serviceContext = getServiceContext();
        auto storageMock = std::make_unique<repl::StorageInterfaceMock>();
        repl::DropPendingCollectionReaper::set(
            serviceContext, std::make_unique<repl::DropPendingCollectionReaper>(storageMock.get()));
        repl::StorageInterface::set(serviceContext, std::move(storageMock));

        _controller = std::make_shared<DonorStateTransitionController>();
        _opObserverRegistry->addObserver(std::make_unique<OpObserverForTest>(_controller));
    }

    void stepUp() {
        auto opCtx = cc().makeOperationContext();
        PrimaryOnlyServiceMongoDTest::stepUp(opCtx.get());
    }

    DonorStateTransitionController* controller() {
        return _controller.get();
    }

    ReshardingDonorDocument makeStateDocument() {
        DonorShardContext donorCtx;
        donorCtx.setState(DonorStateEnum::kPreparingToDonate);

        ReshardingDonorDocument doc(
            std::move(donorCtx),
            {ShardId{"recipient1"}, ShardId{"recipient2"}, ShardId{"recipient3"}});

        NamespaceString sourceNss("sourcedb.sourcecollection");
        auto sourceUUID = UUID::gen();
        auto commonMetadata =
            CommonReshardingMetadata(UUID::gen(),
                                     sourceNss,
                                     sourceUUID,
                                     constructTemporaryReshardingNss(sourceNss.db(), sourceUUID),
                                     BSON("newKey" << 1));

        doc.setCommonReshardingMetadata(std::move(commonMetadata));
        return doc;
    }

    void createOriginalCollection(OperationContext* opCtx,
                                  const ReshardingDonorDocument& donorDoc) {
        CollectionOptions options;
        options.uuid = donorDoc.getSourceUUID();
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            opCtx);
        resharding::data_copy::ensureCollectionExists(opCtx, donorDoc.getSourceNss(), options);
    }

    void notifyRecipientsDoneCloning(OperationContext* opCtx,
                                     DonorStateMachine& donor,
                                     const ReshardingDonorDocument& donorDoc) {
        _onReshardingFieldsChanges(opCtx, donor, donorDoc, CoordinatorStateEnum::kApplying);
    }

    void notifyToStartBlockingWrites(OperationContext* opCtx,
                                     DonorStateMachine& donor,
                                     const ReshardingDonorDocument& donorDoc) {
        _onReshardingFieldsChanges(opCtx, donor, donorDoc, CoordinatorStateEnum::kBlockingWrites);
    }

    void notifyReshardingOutcomeDecided(OperationContext* opCtx,
                                        DonorStateMachine& donor,
                                        const ReshardingDonorDocument& donorDoc,
                                        Status outcome) {
        if (outcome.isOK()) {
            _onReshardingFieldsChanges(
                opCtx, donor, donorDoc, CoordinatorStateEnum::kDecisionPersisted);
        } else {
            _onReshardingFieldsChanges(
                opCtx, donor, donorDoc, CoordinatorStateEnum::kError, std::move(outcome));
        }
    }

private:
    void _onReshardingFieldsChanges(OperationContext* opCtx,
                                    DonorStateMachine& donor,
                                    const ReshardingDonorDocument& donorDoc,
                                    CoordinatorStateEnum coordinatorState,
                                    boost::optional<Status> abortReason = boost::none) {
        auto reshardingFields = TypeCollectionReshardingFields{donorDoc.getReshardingUUID()};
        auto donorFields = TypeCollectionDonorFields{donorDoc.getTempReshardingNss(),
                                                     donorDoc.getReshardingKey(),
                                                     donorDoc.getRecipientShards()};
        reshardingFields.setDonorFields(donorFields);
        reshardingFields.setState(coordinatorState);
        emplaceAbortReasonIfExists(reshardingFields, std::move(abortReason));
        donor.onReshardingFieldsChanges(opCtx, reshardingFields);
    }

    std::shared_ptr<DonorStateTransitionController> _controller;
};

TEST_F(ReshardingDonorServiceTest, CanTransitionThroughEachStateToCompletion) {
    auto doc = makeStateDocument();
    auto opCtx = makeOperationContext();
    DonorStateMachine::insertStateDocument(opCtx.get(), doc);
    auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

    notifyRecipientsDoneCloning(opCtx.get(), *donor, doc);
    notifyToStartBlockingWrites(opCtx.get(), *donor, doc);
    notifyReshardingOutcomeDecided(opCtx.get(), *donor, doc, Status::OK());

    ASSERT_OK(donor->getCompletionFuture().getNoThrow());
}

TEST_F(ReshardingDonorServiceTest, WritesNoOpOplogEntryToGenerateMinFetchTimestamp) {
    boost::optional<PauseDuringStateTransitions> donatingInitialDataTransitionGuard =
        PauseDuringStateTransitions{controller(), {DonorStateEnum::kDonatingInitialData}};

    auto doc = makeStateDocument();
    auto opCtx = makeOperationContext();
    DonorStateMachine::insertStateDocument(opCtx.get(), doc);
    auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

    donatingInitialDataTransitionGuard->wait(DonorStateEnum::kDonatingInitialData);
    stepDown();
    donatingInitialDataTransitionGuard.reset();

    ASSERT_EQ(donor->getCompletionFuture().getNoThrow(),
              ErrorCodes::InterruptedDueToReplStateChange);

    DBDirectClient client(opCtx.get());
    auto cursor =
        client.query(NamespaceString(NamespaceString::kRsOplogNamespace.ns()),
                     BSON("ns" << NamespaceString::kForceOplogBatchBoundaryNamespace.ns()));

    ASSERT_TRUE(cursor->more()) << "Found no oplog entries for source collection";
    repl::OplogEntry op(cursor->next());
    ASSERT_FALSE(cursor->more()) << "Found multiple oplog entries for source collection: "
                                 << op.getEntry() << " and " << cursor->nextSafe();

    ASSERT_EQ(OpType_serializer(op.getOpType()), OpType_serializer(repl::OpTypeEnum::kNoop))
        << op.getEntry();
    ASSERT_FALSE(op.getUuid()) << op.getEntry();
    ASSERT_EQ(op.getObject()["msg"].type(), BSONType::String) << op.getEntry();
    ASSERT_FALSE(bool(op.getObject2())) << op.getEntry();
    ASSERT_FALSE(bool(op.getDestinedRecipient())) << op.getEntry();
}

TEST_F(ReshardingDonorServiceTest, WritesFinalReshardOpOplogEntriesWhileWritesBlocked) {
    boost::optional<PauseDuringStateTransitions> blockingWritesTransitionGuard =
        PauseDuringStateTransitions{controller(), {DonorStateEnum::kBlockingWrites}};

    auto doc = makeStateDocument();
    auto opCtx = makeOperationContext();
    DonorStateMachine::insertStateDocument(opCtx.get(), doc);
    auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

    notifyRecipientsDoneCloning(opCtx.get(), *donor, doc);
    notifyToStartBlockingWrites(opCtx.get(), *donor, doc);

    blockingWritesTransitionGuard->wait(DonorStateEnum::kBlockingWrites);
    stepDown();
    blockingWritesTransitionGuard.reset();

    ASSERT_EQ(donor->getCompletionFuture().getNoThrow(),
              ErrorCodes::InterruptedDueToReplStateChange);

    DBDirectClient client(opCtx.get());
    auto cursor = client.query(NamespaceString(NamespaceString::kRsOplogNamespace.ns()),
                               BSON("ns" << doc.getSourceNss().toString()));

    ASSERT_TRUE(cursor->more()) << "Found no oplog entries for source collection";

    for (const auto& recipientShardId : doc.getRecipientShards()) {
        ASSERT_TRUE(cursor->more()) << "Didn't find finalReshardOp entry for source collection";
        repl::OplogEntry op(cursor->next());

        ASSERT_EQ(OpType_serializer(op.getOpType()), OpType_serializer(repl::OpTypeEnum::kNoop))
            << op.getEntry();
        ASSERT_EQ(op.getUuid(), doc.getSourceUUID()) << op.getEntry();
        ASSERT_EQ(op.getDestinedRecipient(), recipientShardId) << op.getEntry();
        ASSERT_EQ(op.getObject()["msg"].type(), BSONType::String) << op.getEntry();
        ASSERT_TRUE(bool(op.getObject2())) << op.getEntry();
        ASSERT_BSONOBJ_BINARY_EQ(*op.getObject2(),
                                 BSON("type"
                                      << "reshardFinalOp"
                                      << "reshardingUUID" << doc.getReshardingUUID()));
    }

    ASSERT_FALSE(cursor->more()) << "Found extra oplog entry for source collection: "
                                 << cursor->nextSafe();
}

// TODO SERVER-56174: Re-enable this test once the issue with PauseDuringStateTransitions is
// resolved.
/**
TEST_F(ReshardingDonorServiceTest, StepDownStepUpEachTransition) {
    const std::vector<DonorStateEnum> _donorStates{DonorStateEnum::kDonatingInitialData,
                                                   DonorStateEnum::kDonatingOplogEntries,
                                                   DonorStateEnum::kPreparingToBlockWrites,
                                                   DonorStateEnum::kBlockingWrites,
                                                   DonorStateEnum::kDone};
    boost::optional<PauseDuringStateTransitions> stateTransitionsGuard =
        PauseDuringStateTransitions{controller(), _donorStates};
    auto doc = makeStateDocument();
    {
        auto opCtx = makeOperationContext();
        DonorStateMachine::insertStateDocument(opCtx.get(), doc);
    }

    auto prevState = DonorStateEnum::kUnused;
    for (const auto state : _donorStates) {
        {
            auto opCtx = makeOperationContext();
            auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

            if (prevState != DonorStateEnum::kUnused) {
                // Allow the transition to prevState to succeed on this primary-only service
                // instance.
                stateTransitionsGuard->unset(prevState);
            }

            // Signal a change in the coordinator's state for donor state transitions dependent
            // on it.
            switch (state) {
                case DonorStateEnum::kDonatingOplogEntries: {
                    notifyRecipientsDoneCloning(opCtx.get(), *donor, doc);
                    break;
                }
                case DonorStateEnum::kPreparingToBlockWrites: {
                    notifyToStartBlockingWrites(opCtx.get(), *donor, doc);
                    break;
                }
                case DonorStateEnum::kDone: {
                    notifyReshardingOutcomeDecided(opCtx.get(), *donor, doc, Status::OK());
                    break;
                }
                default:
                    break;
            }

            // Step down before the transition to state can complete.
            stateTransitionsGuard->wait(state);
            stepDown();

            ASSERT_EQ(donor->getCompletionFuture().getNoThrow(),
                      ErrorCodes::InterruptedDueToReplStateChange);

            prevState = state;
        }

        stepUp();
    }

    // Finally complete the operation and ensure its success.
    {
        auto opCtx = makeOperationContext();
        auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        stateTransitionsGuard->unset(DonorStateEnum::kDone);
        notifyReshardingOutcomeDecided(opCtx.get(), *donor, doc, Status::OK());
        ASSERT_OK(donor->getCompletionFuture().getNoThrow());
    }
}
*/

TEST_F(ReshardingDonorServiceTest, DropsSourceCollectionWhenDone) {
    auto doc = makeStateDocument();
    auto opCtx = makeOperationContext();

    createOriginalCollection(opCtx.get(), doc);

    DonorStateMachine::insertStateDocument(opCtx.get(), doc);
    auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

    notifyRecipientsDoneCloning(opCtx.get(), *donor, doc);
    notifyToStartBlockingWrites(opCtx.get(), *donor, doc);

    {
        AutoGetCollection coll(opCtx.get(), doc.getSourceNss(), MODE_IS);
        ASSERT_TRUE(bool(coll));
        ASSERT_EQ(coll->uuid(), doc.getSourceUUID());
    }

    notifyReshardingOutcomeDecided(opCtx.get(), *donor, doc, Status::OK());
    ASSERT_OK(donor->getCompletionFuture().getNoThrow());

    {
        AutoGetCollection coll(opCtx.get(), doc.getSourceNss(), MODE_IS);
        ASSERT_FALSE(bool(coll));
    }
}

TEST_F(ReshardingDonorServiceTest, CompletesWithStepdownAfterError) {
    boost::optional<PauseDuringStateTransitions> stateTransitionsGuard =
        PauseDuringStateTransitions{controller(), {DonorStateEnum::kDone}};
    auto doc = makeStateDocument();
    {
        auto opCtx = makeOperationContext();

        createOriginalCollection(opCtx.get(), doc);

        DonorStateMachine::insertStateDocument(opCtx.get(), doc);
        auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyRecipientsDoneCloning(opCtx.get(), *donor, doc);
        notifyReshardingOutcomeDecided(opCtx.get(), *donor, doc, {ErrorCodes::InternalError, ""});

        stateTransitionsGuard->wait(DonorStateEnum::kDone);
        stepDown();

        ASSERT_EQ(donor->getCompletionFuture().getNoThrow(),
                  ErrorCodes::InterruptedDueToReplStateChange);
    }
    stepUp();
    {
        auto opCtx = makeOperationContext();
        auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        stateTransitionsGuard->unset(DonorStateEnum::kDone);

        notifyReshardingOutcomeDecided(opCtx.get(), *donor, doc, {ErrorCodes::InternalError, ""});
        ASSERT_EQ(donor->getCompletionFuture().getNoThrow(), ErrorCodes::InternalError);
        {
            // Verify original collection still exists even with stepdown.
            AutoGetCollection coll(opCtx.get(), doc.getSourceNss(), MODE_IS);
            ASSERT_TRUE(bool(coll));
            ASSERT_EQ(coll->uuid(), doc.getSourceUUID());
        }
    }
}

TEST_F(ReshardingDonorServiceTest, RetainsSourceCollectionOnError) {
    auto doc = makeStateDocument();
    auto opCtx = makeOperationContext();

    createOriginalCollection(opCtx.get(), doc);

    DonorStateMachine::insertStateDocument(opCtx.get(), doc);
    auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

    notifyRecipientsDoneCloning(opCtx.get(), *donor, doc);
    notifyToStartBlockingWrites(opCtx.get(), *donor, doc);

    {
        AutoGetCollection coll(opCtx.get(), doc.getSourceNss(), MODE_IS);
        ASSERT_TRUE(bool(coll));
        ASSERT_EQ(coll->uuid(), doc.getSourceUUID());
    }

    notifyReshardingOutcomeDecided(opCtx.get(), *donor, doc, {ErrorCodes::InternalError, ""});
    ASSERT_EQ(donor->getCompletionFuture().getNoThrow(), ErrorCodes::InternalError);

    {
        AutoGetCollection coll(opCtx.get(), doc.getSourceNss(), MODE_IS);
        ASSERT_TRUE(bool(coll));
        ASSERT_EQ(coll->uuid(), doc.getSourceUUID());
    }
}

}  // namespace
}  // namespace mongo
