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


#include "mongo/db/s/resharding/resharding_donor_service.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/s/resharding/resharding_change_event_o2_field_gen.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_service_test_helpers.h"
#include "mongo/db/s/resharding/resharding_test_util.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <initializer_list>
#include <ostream>
#include <string>
#include <utility>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

using DonorStateTransitionController =
    resharding_service_test_helpers::StateTransitionController<DonorStateEnum>;
using OpObserverForTest =
    resharding_service_test_helpers::StateTransitionControllerOpObserver<DonorStateEnum,
                                                                         ReshardingDonorDocument>;
using PauseDuringStateTransitions =
    resharding_service_test_helpers::PauseDuringStateTransitions<DonorStateEnum>;

const ShardId donorShardId{"myShardId"};

class ExternalStateForTestImpl {
public:
    enum class ExternalFunction {
        kRefreshCollectionPlacementInfo,
        kAbortUnpreparedTransactionIfNecessary,
    };

    ShardId myShardId(ServiceContext* serviceContext) const {
        return donorShardId;
    }

    void waitForCollectionFlush(OperationContext* opCtx, const NamespaceString& nss) {}

    void updateCoordinatorDocument(OperationContext* opCtx,
                                   const BSONObj& query,
                                   const BSONObj& update) {}

    void refreshCollectionPlacementInfo(OperationContext* opCtx, const NamespaceString& sourceNss) {
        _maybeThrowErrorForFunction(opCtx, ExternalFunction::kRefreshCollectionPlacementInfo);
    }

    std::unique_ptr<ShardingRecoveryService::BeforeReleasingCustomAction>
    getOnReleaseCriticalSectionCustomAction() {
        return std::make_unique<ShardingRecoveryService::NoCustomAction>();
    }

    void abortUnpreparedTransactionIfNecessary(OperationContext* opCtx) {
        _maybeThrowErrorForFunction(opCtx,
                                    ExternalFunction::kAbortUnpreparedTransactionIfNecessary);
    }

    void throwUnrecoverableErrorIn(DonorStateEnum phase, ExternalFunction func) {
        _errorFunction = std::make_tuple(phase, func);
    }

private:
    boost::optional<std::tuple<DonorStateEnum, ExternalFunction>> _errorFunction = boost::none;

    DonorStateEnum _getCurrentPhaseOnDisk(OperationContext* opCtx) {
        DBDirectClient client(opCtx);

        auto doc = client.findOne(NamespaceString::kDonorReshardingOperationsNamespace, BSONObj{});
        auto mutableState = doc.getObjectField("mutableState");
        return DonorState_parse(mutableState.getStringField("state"),
                                IDLParserContext{"reshardingDonorServiceTest"});
    }

    void _maybeThrowErrorForFunction(OperationContext* opCtx, ExternalFunction func) {
        if (_errorFunction) {
            auto [expectedPhase, expectedFunction] = *_errorFunction;

            if (_getCurrentPhaseOnDisk(opCtx) == expectedPhase && func == expectedFunction) {
                uasserted(ErrorCodes::InternalError, "Simulating unrecoverable error for testing");
            }
        }
    }
};

class ExternalStateForTest : public ReshardingDonorService::DonorStateMachineExternalState {
public:
    ExternalStateForTest(std::shared_ptr<ExternalStateForTestImpl> impl =
                             std::make_shared<ExternalStateForTestImpl>())
        : _impl(impl) {}

    ShardId myShardId(ServiceContext* serviceContext) const override {
        return _impl->myShardId(serviceContext);
    }

    void waitForCollectionFlush(OperationContext* opCtx, const NamespaceString& nss) override {
        _impl->waitForCollectionFlush(opCtx, nss);
    }

    void updateCoordinatorDocument(OperationContext* opCtx,
                                   const BSONObj& query,
                                   const BSONObj& update) override {
        _impl->updateCoordinatorDocument(opCtx, query, update);
    }

    void refreshCollectionPlacementInfo(OperationContext* opCtx,
                                        const NamespaceString& sourceNss) override {

        _impl->refreshCollectionPlacementInfo(opCtx, sourceNss);
    }

    std::unique_ptr<ShardingRecoveryService::BeforeReleasingCustomAction>
    getOnReleaseCriticalSectionCustomAction() override {
        return _impl->getOnReleaseCriticalSectionCustomAction();
    }

    void abortUnpreparedTransactionIfNecessary(OperationContext* opCtx) override {
        _impl->abortUnpreparedTransactionIfNecessary(opCtx);
    }

private:
    std::shared_ptr<ExternalStateForTestImpl> _impl;
};

struct TestOptions {
    bool isAlsoRecipient;
    bool performVerification = true;

    BSONObj toBSON() const {
        BSONObjBuilder bob;
        bob.append("isAlsoRecipient", isAlsoRecipient);
        bob.append("performVerification", performVerification);
        return bob.obj();
    }
};

std::vector<TestOptions> makeAllTestOptions() {
    std::vector<TestOptions> testOptions;
    for (bool isAlsoRecipient : {false, true}) {
        for (bool performVerification : {false, true}) {
            testOptions.push_back({isAlsoRecipient, performVerification});
        }
    }
    return testOptions;
}

class ReshardingDonorServiceForTest : public ReshardingDonorService {
public:
    explicit ReshardingDonorServiceForTest(
        ServiceContext* serviceContext,
        std::shared_ptr<ExternalStateForTestImpl> externalStateImpl =
            std::make_shared<ExternalStateForTestImpl>())
        : ReshardingDonorService(serviceContext),
          _serviceContext(serviceContext),
          _externalStateImpl(externalStateImpl) {}

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialState) override {
        return std::make_shared<DonorStateMachine>(
            this,
            ReshardingDonorDocument::parse(initialState,
                                           IDLParserContext{"ReshardingDonorServiceForTest"}),
            std::make_unique<ExternalStateForTest>(_externalStateImpl),
            _serviceContext);
    }

private:
    ServiceContext* _serviceContext;
    std::shared_ptr<ExternalStateForTestImpl> _externalStateImpl;
};

class ReshardingDonorServiceTest : public repl::PrimaryOnlyServiceMongoDTest {
public:
    using DonorStateMachine = ReshardingDonorService::DonorStateMachine;
    using enum ExternalStateForTestImpl::ExternalFunction;

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<ReshardingDonorServiceForTest>(serviceContext, _externalStateImpl);
    }

    void setUp() override {
        _externalStateImpl = std::make_shared<ExternalStateForTestImpl>();
        repl::PrimaryOnlyServiceMongoDTest::setUp();

        auto serviceContext = getServiceContext();
        auto storageMock = std::make_unique<repl::StorageInterfaceMock>();
        repl::StorageInterface::set(serviceContext, std::move(storageMock));

        _controller = std::make_shared<DonorStateTransitionController>();
        _opObserverRegistry->addObserver(std::make_unique<OpObserverForTest>(
            _controller,
            NamespaceString::kDonorReshardingOperationsNamespace,
            [](const ReshardingDonorDocument& donorDoc) {
                return donorDoc.getMutableState().getState();
            }));
    }

    DonorStateTransitionController* controller() {
        return _controller.get();
    }

    ExternalStateForTestImpl* externalState() {
        return _externalStateImpl.get();
    }

    ReshardingDonorDocument makeStateDocument(const TestOptions& testOptions) {
        DonorShardContext donorCtx;
        donorCtx.setState(DonorStateEnum::kPreparingToDonate);

        ReshardingDonorDocument doc(
            std::move(donorCtx),
            {ShardId{"recipient1"},
             testOptions.isAlsoRecipient ? donorShardId : ShardId{"recipient2"},
             ShardId{"recipient3"}});

        NamespaceString sourceNss =
            NamespaceString::createNamespaceString_forTest("sourcedb.sourcecollection");
        auto sourceUUID = UUID::gen();
        auto commonMetadata = CommonReshardingMetadata(
            UUID::gen(),
            sourceNss,
            sourceUUID,
            resharding::constructTemporaryReshardingNss(sourceNss, sourceUUID),
            BSON("newKey" << 1));
        commonMetadata.setStartTime(getServiceContext()->getFastClockSource()->now());
        commonMetadata.setPerformVerification(testOptions.performVerification);

        doc.setCommonReshardingMetadata(std::move(commonMetadata));
        return doc;
    }

    void createSourceCollection(OperationContext* opCtx, const ReshardingDonorDocument& donorDoc) {
        CollectionOptions options;
        options.uuid = donorDoc.getSourceUUID();
        resharding::data_copy::ensureCollectionDropped(opCtx, donorDoc.getSourceNss());
        resharding::data_copy::ensureCollectionExists(opCtx, donorDoc.getSourceNss(), options);

        cloneTimestamp =
            repl::ReplicationCoordinator::get(opCtx)->getMyLastAppliedOpTime().getTimestamp();
    }

    void writeToSourceCollection(OperationContext* opCtx,
                                 const ReshardingDonorDocument& donorDoc,
                                 int numInserts,
                                 int numUpdates,
                                 int numDeletes) {
        ASSERT(numInserts >= numUpdates);
        ASSERT(numInserts >= numDeletes);

        DBDirectClient client(opCtx);

        for (int i = 0; i <= numInserts; i++) {
            client.insert(donorDoc.getSourceNss(), BSON("_id" << i << "x" << i));
        }
        for (int i = 0; i <= numUpdates; i++) {
            client.update(donorDoc.getSourceNss(),
                          BSON("_id" << 0),
                          BSON("$inc" << BSON("x" << 1)),
                          false /*upsert*/,
                          false /*multi*/);
        }
        for (int i = 0; i <= numDeletes; i++) {
            client.remove(donorDoc.getSourceNss(), BSON("_id" << i), false);
        }
    }

    void createTemporaryReshardingCollection(OperationContext* opCtx,
                                             const ReshardingDonorDocument& donorDoc) {
        CollectionOptions options;
        options.uuid = donorDoc.getReshardingUUID();
        resharding::data_copy::ensureCollectionDropped(opCtx, donorDoc.getTempReshardingNss());
        resharding::data_copy::ensureCollectionExists(
            opCtx, donorDoc.getTempReshardingNss(), options);
    }

    void awaitDonorState(OperationContext* opCtx,
                         const UUID& reshardingUUID,
                         DonorStateEnum minState) {
        resharding_test_util::assertSoon(opCtx, [&] {
            auto persistedDonorDoc = getPersistedDonorDocumentOptional(opCtx, reshardingUUID);
            return persistedDonorDoc &&
                (persistedDonorDoc->getMutableState().getState() == minState);
        });
    }

    void notifyToStartChangeStreamsMonitor(OperationContext* opCtx,
                                           DonorStateMachine& donor,
                                           const ReshardingDonorDocument& donorDoc) {
        // Wait for the donor to be in the "donating-initial-data" state since the change streams
        // monitor should only start after the clone timestamp has been chosen.
        awaitDonorState(opCtx, donorDoc.getReshardingUUID(), DonorStateEnum::kDonatingInitialData);

        auto status = donor.createAndStartChangeStreamsMonitor(cloneTimestamp).getNoThrow(opCtx);
        if (donorDoc.getPerformVerification()) {
            ASSERT_OK(status);
            checkChangeStreamsMonitor(opCtx, donorDoc, boost::none /* documentsDelta */);
            // Perform some writes against the source collection so we can verify the delta later
            // in the test.
            writeToSourceCollection(opCtx, donorDoc, _numInserts, _numUpdates, _numDeletes);
        } else {
            ASSERT_EQ(status, ErrorCodes::IllegalOperation);
        }
    }

    void notifyRecipientsDoneCloning(OperationContext* opCtx,
                                     DonorStateMachine& donor,
                                     const ReshardingDonorDocument& donorDoc) {
        _onReshardingFieldsChanges(opCtx, donor, donorDoc, CoordinatorStateEnum::kApplying);
    }

    void notifyToStartBlockingWrites(OperationContext* opCtx,
                                     DonorStateMachine& donor,
                                     const ReshardingDonorDocument& donorDoc) {
        notifyToStartBlockingWritesNoWait(opCtx, donor, donorDoc);
        ASSERT_OK(donor.awaitCriticalSectionAcquired().waitNoThrow(opCtx));
    }

    void notifyToStartBlockingWritesNoWait(OperationContext* opCtx,
                                           DonorStateMachine& donor,
                                           const ReshardingDonorDocument& donorDoc) {
        _onReshardingFieldsChanges(opCtx, donor, donorDoc, CoordinatorStateEnum::kBlockingWrites);
    }

    void awaitChangeStreamsMonitorCompleted(OperationContext* opCtx,
                                            DonorStateMachine& donor,
                                            const ReshardingDonorDocument& donorDoc) {
        auto swDocumentsDelta = donor.awaitChangeStreamsMonitorCompleted().getNoThrow(opCtx);
        if (donorDoc.getPerformVerification()) {
            ASSERT_OK(swDocumentsDelta.getStatus());
            // Verify the delta.
            auto documentsDelta = _numInserts - _numDeletes;
            ASSERT_EQ(swDocumentsDelta.getValue(), documentsDelta);
            checkChangeStreamsMonitor(opCtx, donorDoc, documentsDelta);
        } else {
            ASSERT_EQ(swDocumentsDelta.getStatus(), ErrorCodes::IllegalOperation);
        }
    }

    void notifyReshardingCommitting(OperationContext* opCtx,
                                    DonorStateMachine& donor,
                                    const ReshardingDonorDocument& donorDoc) {
        _onReshardingFieldsChanges(opCtx, donor, donorDoc, CoordinatorStateEnum::kCommitting);
        ASSERT_OK(donor.awaitCriticalSectionPromoted().waitNoThrow(opCtx));
    }

    void checkStateDocumentRemoved(OperationContext* opCtx) {
        const auto donorColl = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest{NamespaceString::kDonorReshardingOperationsNamespace,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kRead},
            MODE_IS);
        ASSERT_TRUE(donorColl.exists());
        ASSERT_TRUE(bool(donorColl.getCollectionPtr()->isEmpty(opCtx)));
    }

    boost::optional<ReshardingDonorDocument> getPersistedDonorDocumentOptional(
        OperationContext* opCtx, const UUID& reshardingUUID) {
        boost::optional<ReshardingDonorDocument> doc;
        PersistentTaskStore<ReshardingDonorDocument> store(
            NamespaceString::kDonorReshardingOperationsNamespace);
        store.forEach(opCtx,
                      BSON(ReshardingDonorDocument::kReshardingUUIDFieldName << reshardingUUID),
                      [&](const auto& donorDocument) {
                          doc.emplace(donorDocument);
                          return false;
                      });
        return doc;
    }

    ReshardingDonorDocument getPersistedDonorDocument(OperationContext* opCtx,
                                                      const UUID& reshardingUUID) {
        auto doc = getPersistedDonorDocumentOptional(opCtx, reshardingUUID);
        ASSERT(doc);
        return doc.get();
    }

    void checkChangeStreamsMonitor(OperationContext* opCtx,
                                   const ReshardingDonorDocument& donorDoc,
                                   boost::optional<int64_t> documentsDelta) {
        auto persistedDonorDoc = getPersistedDonorDocument(opCtx, donorDoc.getReshardingUUID());
        auto changeStreamsMonitorCtx = persistedDonorDoc.getChangeStreamsMonitor();
        ASSERT(changeStreamsMonitorCtx);
        ASSERT_EQ(changeStreamsMonitorCtx->getStartAtOperationTime(), cloneTimestamp + 1);
        if (documentsDelta) {
            ASSERT_EQ(changeStreamsMonitorCtx->getDocumentsDelta(), *documentsDelta);
        }
    }

    void runUnrecoverableErrorTest(const TestOptions& testOptions, DonorStateEnum state) {
        auto doc = makeStateDocument(testOptions);
        auto instanceId =
            BSON(ReshardingDonorDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();
        createSourceCollection(opCtx.get(), doc);
        if (testOptions.isAlsoRecipient) {
            createTemporaryReshardingCollection(opCtx.get(), doc);
        }

        DonorStateMachine::insertStateDocument(opCtx.get(), doc);
        auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        _advanceDonorToState(opCtx.get(), *donor, doc, state);

        ASSERT_OK(donor->awaitInBlockingWritesOrError().getNoThrow());

        auto persistedDoc = getPersistedDonorDocument(opCtx.get(), doc.getReshardingUUID());
        ASSERT_EQ(persistedDoc.getMutableState().getState(), DonorStateEnum::kError);

        auto abortReason = persistedDoc.getMutableState().getAbortReason();
        ASSERT(abortReason);
        ASSERT_EQ(abortReason->getIntField("code"), ErrorCodes::InternalError);

        donor->abort(false);
        ASSERT_OK(donor->getCompletionFuture().getNoThrow());
        checkStateDocumentRemoved(opCtx.get());
    }

private:
    void _onReshardingFieldsChanges(OperationContext* opCtx,
                                    DonorStateMachine& donor,
                                    const ReshardingDonorDocument& donorDoc,
                                    CoordinatorStateEnum coordinatorState) {
        auto reshardingFields = TypeCollectionReshardingFields{donorDoc.getReshardingUUID()};
        auto donorFields = TypeCollectionDonorFields{donorDoc.getTempReshardingNss(),
                                                     donorDoc.getReshardingKey(),
                                                     donorDoc.getRecipientShards()};
        reshardingFields.setDonorFields(donorFields);
        reshardingFields.setState(coordinatorState);
        donor.onReshardingFieldsChanges(opCtx, reshardingFields);
    }

    void _advanceDonorToState(OperationContext* opCtx,
                              DonorStateMachine& donor,
                              const ReshardingDonorDocument& donorDoc,
                              DonorStateEnum targetState) {
        if (targetState >= DonorStateEnum::kDonatingInitialData) {
            notifyToStartChangeStreamsMonitor(opCtx, donor, donorDoc);
        }

        if (targetState >= DonorStateEnum::kDonatingOplogEntries) {
            notifyRecipientsDoneCloning(opCtx, donor, donorDoc);
        }

        if (targetState >= DonorStateEnum::kPreparingToBlockWrites) {
            notifyToStartBlockingWritesNoWait(opCtx, donor, donorDoc);
        }
    }

    std::shared_ptr<DonorStateTransitionController> _controller;
    std::shared_ptr<ExternalStateForTestImpl> _externalStateImpl;

    // The number of writes after the clone timestamp.
    const int64_t _numInserts = 5;
    const int64_t _numUpdates = 1;
    const int64_t _numDeletes = 2;

    // Set the batch size 1 to test multi-batch processing in unit tests with multiple events.
    RAIIServerParameterControllerForTest _batchSize{
        "reshardingVerificationChangeStreamsEventsBatchSizeLimit", 1};

protected:
    // The clone timestamp is set per test case after creating the source collection.
    Timestamp cloneTimestamp;
};

TEST_F(ReshardingDonorServiceTest, CanTransitionThroughEachStateToCompletion) {
    for (auto& testOptions : makeAllTestOptions()) {
        LOGV2(5641800,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        auto doc = makeStateDocument(testOptions);
        auto opCtx = makeOperationContext();

        createSourceCollection(opCtx.get(), doc);
        if (testOptions.isAlsoRecipient) {
            createTemporaryReshardingCollection(opCtx.get(), doc);
        }

        DonorStateMachine::insertStateDocument(opCtx.get(), doc);
        auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartChangeStreamsMonitor(opCtx.get(), *donor, doc);
        notifyRecipientsDoneCloning(opCtx.get(), *donor, doc);
        notifyToStartBlockingWrites(opCtx.get(), *donor, doc);
        awaitChangeStreamsMonitorCompleted(opCtx.get(), *donor, doc);
        notifyReshardingCommitting(opCtx.get(), *donor, doc);

        ASSERT_OK(donor->getCompletionFuture().getNoThrow());
        checkStateDocumentRemoved(opCtx.get());
    }
}

TEST_F(ReshardingDonorServiceTest, WritesNoOpOplogEntryOnReshardingBegin) {
    boost::optional<PauseDuringStateTransitions> donatingInitialDataTransitionGuard;
    donatingInitialDataTransitionGuard.emplace(controller(), DonorStateEnum::kDonatingInitialData);

    auto doc = makeStateDocument({.isAlsoRecipient = false});
    auto opCtx = makeOperationContext();
    auto rawOpCtx = opCtx.get();
    DonorStateMachine::insertStateDocument(rawOpCtx, doc);
    auto donor = DonorStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());

    donatingInitialDataTransitionGuard->wait(DonorStateEnum::kDonatingInitialData);
    stepDown();
    donatingInitialDataTransitionGuard.reset();
    ASSERT_EQ(donor->getCompletionFuture().getNoThrow(),
              ErrorCodes::InterruptedDueToReplStateChange);

    DBDirectClient client(opCtx.get());
    NamespaceString sourceNss =
        NamespaceString::createNamespaceString_forTest("sourcedb", "sourcecollection");
    FindCommandRequest findRequest{NamespaceString::kRsOplogNamespace};
    findRequest.setFilter(BSON("ns" << sourceNss.toString_forTest()));
    auto cursor = client.find(std::move(findRequest));

    ASSERT_TRUE(cursor->more()) << "Found no oplog entries for source collection";
    repl::OplogEntry op(cursor->next());
    ASSERT_FALSE(cursor->more()) << "Found multiple oplog entries for source collection: "
                                 << op.getEntry() << " and " << cursor->nextSafe();

    ReshardBeginChangeEventO2Field expectedChangeEvent{sourceNss, doc.getReshardingUUID()};
    auto receivedChangeEvent = ReshardBeginChangeEventO2Field::parse(
        *op.getObject2(), IDLParserContext("ReshardBeginChangeEventO2Field"));

    ASSERT_EQ(OpType_serializer(op.getOpType()), OpType_serializer(repl::OpTypeEnum::kNoop))
        << op.getEntry();
    ASSERT_EQ(*op.getUuid(), doc.getSourceUUID()) << op.getEntry();
    ASSERT_EQ(op.getObject()["msg"].type(), BSONType::string) << op.getEntry();
    ASSERT_EQ(receivedChangeEvent, expectedChangeEvent);
    ASSERT_TRUE(op.getFromMigrate());
    ASSERT_FALSE(bool(op.getDestinedRecipient())) << op.getEntry();
}

TEST_F(ReshardingDonorServiceTest, WritesNoOpOplogEntryToGenerateMinFetchTimestamp) {
    boost::optional<PauseDuringStateTransitions> donatingInitialDataTransitionGuard;
    donatingInitialDataTransitionGuard.emplace(controller(), DonorStateEnum::kDonatingInitialData);

    auto doc = makeStateDocument({.isAlsoRecipient = false});
    auto opCtx = makeOperationContext();
    DonorStateMachine::insertStateDocument(opCtx.get(), doc);
    auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

    donatingInitialDataTransitionGuard->wait(DonorStateEnum::kDonatingInitialData);
    stepDown();
    donatingInitialDataTransitionGuard.reset();

    ASSERT_EQ(donor->getCompletionFuture().getNoThrow(),
              ErrorCodes::InterruptedDueToReplStateChange);

    DBDirectClient client(opCtx.get());
    FindCommandRequest findRequest{NamespaceString::kRsOplogNamespace};
    findRequest.setFilter(
        BSON("ns" << redactTenant(NamespaceString::kForceOplogBatchBoundaryNamespace)));
    auto cursor = client.find(std::move(findRequest));

    ASSERT_TRUE(cursor->more()) << "Found no oplog entries for source collection";
    repl::OplogEntry op(cursor->next());
    ASSERT_FALSE(cursor->more()) << "Found multiple oplog entries for source collection: "
                                 << op.getEntry() << " and " << cursor->nextSafe();

    ASSERT_EQ(OpType_serializer(op.getOpType()), OpType_serializer(repl::OpTypeEnum::kNoop))
        << op.getEntry();
    ASSERT_FALSE(op.getUuid()) << op.getEntry();
    ASSERT_EQ(op.getObject()["msg"].type(), BSONType::string) << op.getEntry();
    ASSERT_FALSE(bool(op.getObject2())) << op.getEntry();
    ASSERT_FALSE(bool(op.getDestinedRecipient())) << op.getEntry();
}

TEST_F(ReshardingDonorServiceTest, WritesFinalReshardOpOplogEntriesWhileWritesBlocked) {
    boost::optional<PauseDuringStateTransitions> blockingWritesTransitionGuard;
    blockingWritesTransitionGuard.emplace(controller(), DonorStateEnum::kBlockingWrites);

    auto doc = makeStateDocument({.isAlsoRecipient = false});
    auto opCtx = makeOperationContext();
    DonorStateMachine::insertStateDocument(opCtx.get(), doc);
    auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

    notifyToStartChangeStreamsMonitor(opCtx.get(), *donor, doc);
    notifyRecipientsDoneCloning(opCtx.get(), *donor, doc);
    notifyToStartBlockingWrites(opCtx.get(), *donor, doc);

    blockingWritesTransitionGuard->wait(DonorStateEnum::kBlockingWrites);
    stepDown();
    blockingWritesTransitionGuard.reset();

    ASSERT_EQ(donor->getCompletionFuture().getNoThrow(),
              ErrorCodes::InterruptedDueToReplStateChange);

    DBDirectClient client(opCtx.get());
    FindCommandRequest findRequest{NamespaceString::kRsOplogNamespace};
    findRequest.setFilter(BSON("o2.type" << resharding::kReshardFinalOpLogType));
    auto cursor = client.find(std::move(findRequest));

    ASSERT_TRUE(cursor->more()) << "Found no oplog entries for source collection";

    ReshardBlockingWritesChangeEventO2Field expectedChangeEvent{
        doc.getSourceNss(),
        doc.getReshardingUUID(),
        std::string{resharding::kReshardFinalOpLogType},
    };

    for (const auto& recipientShardId : doc.getRecipientShards()) {
        ASSERT_TRUE(cursor->more()) << "Didn't find finalReshardOp entry for source collection";
        repl::OplogEntry op(cursor->next());

        auto receivedChangeEvent = ReshardBlockingWritesChangeEventO2Field::parse(
            *op.getObject2(), IDLParserContext("ReshardBlockingWritesChangeEventO2Field"));

        ASSERT_EQ(OpType_serializer(op.getOpType()), OpType_serializer(repl::OpTypeEnum::kNoop))
            << op.getEntry();
        ASSERT_EQ(op.getUuid(), doc.getSourceUUID()) << op.getEntry();
        ASSERT_EQ(op.getDestinedRecipient(), recipientShardId) << op.getEntry();
        ASSERT_EQ(op.getObject()["msg"].type(), BSONType::string) << op.getEntry();
        ASSERT_TRUE(bool(op.getObject2())) << op.getEntry();
        ASSERT_EQ(receivedChangeEvent, expectedChangeEvent);
    }

    ASSERT_FALSE(cursor->more()) << "Found extra oplog entry for source collection: "
                                 << cursor->nextSafe();
}

TEST_F(ReshardingDonorServiceTest, StepDownStepUpEachTransition) {
    const std::vector<DonorStateEnum> donorStates{DonorStateEnum::kDonatingInitialData,
                                                  DonorStateEnum::kDonatingOplogEntries,
                                                  DonorStateEnum::kPreparingToBlockWrites,
                                                  DonorStateEnum::kBlockingWrites,
                                                  DonorStateEnum::kDone};

    const std::vector<std::pair<DonorStateEnum, bool>> donorStateTransitions{
        {DonorStateEnum::kDonatingInitialData, false},
        {DonorStateEnum::kDonatingOplogEntries, false},
        {DonorStateEnum::kPreparingToBlockWrites, false},
        {DonorStateEnum::kBlockingWrites, false},
        {DonorStateEnum::kBlockingWrites, true},
        {DonorStateEnum::kDone, true}};

    for (auto& testOptions : makeAllTestOptions()) {
        LOGV2(5641801,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        PauseDuringStateTransitions stateTransitionsGuard{controller(), donorStates};
        auto doc = makeStateDocument(testOptions);
        auto instanceId =
            BSON(ReshardingDonorDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();

        auto prevState = DonorStateEnum::kUnused;
        for (const auto& [state, critSecHeld] : donorStateTransitions) {
            // The kBlockingWrite state is interrupted twice so we don't unset the guard until after
            // the second time.
            bool shouldUnsetPrevState = !(state == DonorStateEnum::kBlockingWrites && critSecHeld);
            auto donor = [&] {
                if (prevState == DonorStateEnum::kUnused) {
                    createSourceCollection(opCtx.get(), doc);
                    if (testOptions.isAlsoRecipient) {
                        createTemporaryReshardingCollection(opCtx.get(), doc);
                    }

                    DonorStateMachine::insertStateDocument(opCtx.get(), doc);
                    return DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());
                } else {
                    auto [maybeDonor, isPausedOrShutdown] =
                        DonorStateMachine::lookup(opCtx.get(), _service, instanceId);
                    ASSERT_TRUE(maybeDonor);
                    ASSERT_FALSE(isPausedOrShutdown);

                    // Allow the transition to prevState to succeed on this primary-only service
                    // instance.
                    if (shouldUnsetPrevState) {
                        stateTransitionsGuard.unset(prevState);
                    }
                    return *maybeDonor;
                }
            }();

            // Signal a change in the coordinator's state for the donor state transition dependent
            // on it.
            switch (state) {
                case DonorStateEnum::kDonatingOplogEntries: {
                    notifyToStartChangeStreamsMonitor(opCtx.get(), *donor, doc);
                    notifyRecipientsDoneCloning(opCtx.get(), *donor, doc);
                    break;
                }
                case DonorStateEnum::kPreparingToBlockWrites: {
                    notifyToStartBlockingWritesNoWait(opCtx.get(), *donor, doc);
                    break;
                }
                case DonorStateEnum::kBlockingWrites: {
                    // A shard version refresh cannot be triggered once the critical section has
                    // been acquired. We intentionally test the DonorStateEnum::kBlockingWrites
                    // transition being triggered two different ways:
                    //
                    //   - The first transition would wait for the RecoverRefreshThread to
                    //     notify the donor about the CoordinatorStateEnum::kBlockingWrites state.
                    //
                    //   - The second transition would rely on the donor having already written down
                    //     DonorStateEnum::kPreparingToBlockWrites as a result of the
                    //     RecoverRefreshThread having already been notified the donor about the
                    //     CoordinatorStateEnum::kBlockingWrites state before.
                    if (!critSecHeld) {
                        notifyToStartBlockingWrites(opCtx.get(), *donor, doc);
                    }
                    break;
                }
                case DonorStateEnum::kDone: {
                    awaitChangeStreamsMonitorCompleted(opCtx.get(), *donor, doc);
                    notifyReshardingCommitting(opCtx.get(), *donor, doc);
                    break;
                }
                default:
                    break;
            }

            // Step down before the transition to state can complete.
            stateTransitionsGuard.wait(state);
            stepDown();

            ASSERT_EQ(donor->getCompletionFuture().getNoThrow(),
                      ErrorCodes::InterruptedDueToReplStateChange);

            prevState = state;

            donor.reset();
            stepUp(opCtx.get());
        }

        // Finally complete the operation and ensure its success.
        auto [maybeDonor, isPausedOrShutdown] =
            DonorStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(maybeDonor);
        ASSERT_FALSE(isPausedOrShutdown);

        auto donor = *maybeDonor;
        stateTransitionsGuard.unset(DonorStateEnum::kDone);

        awaitChangeStreamsMonitorCompleted(opCtx.get(), *donor, doc);
        notifyReshardingCommitting(opCtx.get(), *donor, doc);
        ASSERT_OK(donor->getCompletionFuture().getNoThrow());
        checkStateDocumentRemoved(opCtx.get());
    }
}

TEST_F(ReshardingDonorServiceTest, ReportForCurrentOpAfterCompletion) {
    const auto donorState = DonorStateEnum::kDonatingInitialData;

    PauseDuringStateTransitions stateTransitionsGuard{controller(), donorState};
    auto doc = makeStateDocument({.isAlsoRecipient = false});
    auto instanceId =
        BSON(ReshardingDonorDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

    auto opCtx = makeOperationContext();
    auto donor = [&] {
        createSourceCollection(opCtx.get(), doc);

        DonorStateMachine::insertStateDocument(opCtx.get(), doc);
        return DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());
    }();

    // Step down before the transition to state can complete.
    stateTransitionsGuard.wait(donorState);
    stepDown();
    stateTransitionsGuard.unset(donorState);

    // At this point, the resharding metrics will have been unregistered from the cumulative metrics
    ASSERT_EQ(donor->getCompletionFuture().getNoThrow(),
              ErrorCodes::InterruptedDueToReplStateChange);

    // Now call step up. The old donor object has not yet been destroyed because we still hold
    // a shared pointer to it ('donor') - this can happen in production after a failover if a
    // state machine is slow to clean up.
    stepUp(opCtx.get());

    // Assert that the old donor object will return a currentOp report, because the resharding
    // metrics still exist on the coordinator object itelf.
    ASSERT(donor->reportForCurrentOp(MongoProcessInterface::CurrentOpConnectionsMode::kExcludeIdle,
                                     MongoProcessInterface::CurrentOpSessionsMode::kIncludeIdle));

    // Ensure the new donor started up successfully (and thus, registered new resharding metrics),
    // despite the "zombie" state machine still existing.
    auto [maybeDonor, isPausedOrShutdown] =
        DonorStateMachine::lookup(opCtx.get(), _service, instanceId);
    ASSERT_TRUE(maybeDonor);
    ASSERT_FALSE(isPausedOrShutdown);
    auto newDonor = *maybeDonor;
    ASSERT_NE(donor, newDonor);

    // No need to finish the resharding op, so we just cancel the op.
    newDonor->abort(false);
    ASSERT_OK(newDonor->getCompletionFuture().getNoThrow());
}

DEATH_TEST_REGEX_F(ReshardingDonorServiceTest, CommitFn, "4457001.*tripwire") {
    for (auto& testOptions : makeAllTestOptions()) {
        LOGV2(9858405,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);
        auto doc = makeStateDocument(testOptions);
        auto opCtx = makeOperationContext();

        createSourceCollection(opCtx.get(), doc);
        if (testOptions.isAlsoRecipient) {
            createTemporaryReshardingCollection(opCtx.get(), doc);
        }

        DonorStateMachine::insertStateDocument(opCtx.get(), doc);
        auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartChangeStreamsMonitor(opCtx.get(), *donor, doc);
        notifyRecipientsDoneCloning(opCtx.get(), *donor, doc);

        ASSERT_THROWS_CODE(donor->commit(), DBException, ErrorCodes::ReshardCollectionInProgress);

        notifyToStartBlockingWrites(opCtx.get(), *donor, doc);
        awaitChangeStreamsMonitorCompleted(opCtx.get(), *donor, doc);

        donor->awaitInBlockingWritesOrError().get();

        donor->commit();

        ASSERT_OK(donor->getCompletionFuture().getNoThrow());
    }
}

TEST_F(ReshardingDonorServiceTest, DropsSourceCollectionWhenDone) {
    auto doc = makeStateDocument({.isAlsoRecipient = false});
    auto opCtx = makeOperationContext();

    createSourceCollection(opCtx.get(), doc);

    DonorStateMachine::insertStateDocument(opCtx.get(), doc);
    auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

    notifyToStartChangeStreamsMonitor(opCtx.get(), *donor, doc);
    notifyRecipientsDoneCloning(opCtx.get(), *donor, doc);
    notifyToStartBlockingWrites(opCtx.get(), *donor, doc);

    {
        const auto coll = acquireCollection(
            opCtx.get(),
            CollectionAcquisitionRequest{doc.getSourceNss(),
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(opCtx.get()),
                                         AcquisitionPrerequisites::kRead},
            MODE_IS);
        ASSERT_TRUE(coll.exists());
        ASSERT_EQ(coll.uuid(), doc.getSourceUUID());
    }

    awaitChangeStreamsMonitorCompleted(opCtx.get(), *donor, doc);
    notifyReshardingCommitting(opCtx.get(), *donor, doc);
    ASSERT_OK(donor->getCompletionFuture().getNoThrow());
    checkStateDocumentRemoved(opCtx.get());

    {
        const auto coll = acquireCollection(
            opCtx.get(),
            CollectionAcquisitionRequest{doc.getSourceNss(),
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(opCtx.get()),
                                         AcquisitionPrerequisites::kRead},
            MODE_IS);
        ASSERT_FALSE(coll.exists());
    }
}

TEST_F(ReshardingDonorServiceTest, RenamesTemporaryReshardingCollectionWhenDone) {
    auto doc = makeStateDocument({.isAlsoRecipient = true});
    auto opCtx = makeOperationContext();

    createSourceCollection(opCtx.get(), doc);
    createTemporaryReshardingCollection(opCtx.get(), doc);

    DonorStateMachine::insertStateDocument(opCtx.get(), doc);
    auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

    notifyToStartChangeStreamsMonitor(opCtx.get(), *donor, doc);
    notifyRecipientsDoneCloning(opCtx.get(), *donor, doc);
    notifyToStartBlockingWrites(opCtx.get(), *donor, doc);

    {
        const auto coll = acquireCollection(
            opCtx.get(),
            CollectionAcquisitionRequest{doc.getSourceNss(),
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(opCtx.get()),
                                         AcquisitionPrerequisites::kRead},
            MODE_IS);
        ASSERT_TRUE(coll.exists());
        ASSERT_EQ(coll.uuid(), doc.getSourceUUID());
    }

    awaitChangeStreamsMonitorCompleted(opCtx.get(), *donor, doc);
    notifyReshardingCommitting(opCtx.get(), *donor, doc);
    ASSERT_OK(donor->getCompletionFuture().getNoThrow());
    checkStateDocumentRemoved(opCtx.get());

    {
        const auto coll = acquireCollection(
            opCtx.get(),
            CollectionAcquisitionRequest{doc.getSourceNss(),
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(opCtx.get()),
                                         AcquisitionPrerequisites::kRead},
            MODE_IS);
        ASSERT_TRUE(coll.exists());
        ASSERT_EQ(coll.uuid(), doc.getReshardingUUID());
    }
}

TEST_F(ReshardingDonorServiceTest, CompletesWithStepdownAfterAbort) {
    for (auto& testOptions : makeAllTestOptions()) {
        LOGV2(5641802,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        boost::optional<PauseDuringStateTransitions> doneTransitionGuard;
        doneTransitionGuard.emplace(controller(), DonorStateEnum::kDone);

        auto doc = makeStateDocument(testOptions);
        auto instanceId =
            BSON(ReshardingDonorDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();

        createSourceCollection(opCtx.get(), doc);
        if (testOptions.isAlsoRecipient) {
            createTemporaryReshardingCollection(opCtx.get(), doc);
        }

        DonorStateMachine::insertStateDocument(opCtx.get(), doc);
        auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartChangeStreamsMonitor(opCtx.get(), *donor, doc);
        notifyRecipientsDoneCloning(opCtx.get(), *donor, doc);
        // The call to notifyToStartBlockingWrites() is skipped here because the donor is being
        // notified that the resharding operation is aborting before the donor would have
        // transitioned to kBlockingWrites.
        donor->abort(false);

        // Step down before the transition to kDone can complete.
        doneTransitionGuard->wait(DonorStateEnum::kDone);
        stepDown();

        ASSERT_EQ(donor->getCompletionFuture().getNoThrow(),
                  ErrorCodes::InterruptedDueToReplStateChange);

        donor.reset();
        stepUp(opCtx.get());

        auto [maybeDonor, isPausedOrShutdown] =
            DonorStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(maybeDonor);
        ASSERT_FALSE(isPausedOrShutdown);

        donor = *maybeDonor;
        doneTransitionGuard.reset();

        donor->abort(false);
        ASSERT_OK(donor->getCompletionFuture().getNoThrow());
        checkStateDocumentRemoved(opCtx.get());

        {
            const auto coll =
                acquireCollection(opCtx.get(),
                                  CollectionAcquisitionRequest{
                                      doc.getSourceNss(),
                                      PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                      repl::ReadConcernArgs::get(opCtx.get()),
                                      AcquisitionPrerequisites::kRead},
                                  MODE_IS);
            ASSERT_TRUE(coll.exists());
            ASSERT_EQ(coll.uuid(), doc.getSourceUUID());
        }
    }
}

TEST_F(ReshardingDonorServiceTest, RetainsSourceCollectionOnAbort) {
    for (auto& testOptions : makeAllTestOptions()) {
        LOGV2(5641803,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        auto doc = makeStateDocument(testOptions);
        auto opCtx = makeOperationContext();

        createSourceCollection(opCtx.get(), doc);
        if (testOptions.isAlsoRecipient) {
            createTemporaryReshardingCollection(opCtx.get(), doc);
        }

        DonorStateMachine::insertStateDocument(opCtx.get(), doc);
        auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartChangeStreamsMonitor(opCtx.get(), *donor, doc);
        notifyRecipientsDoneCloning(opCtx.get(), *donor, doc);
        notifyToStartBlockingWrites(opCtx.get(), *donor, doc);

        {
            const auto coll =
                acquireCollection(opCtx.get(),
                                  CollectionAcquisitionRequest{
                                      doc.getSourceNss(),
                                      PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                      repl::ReadConcernArgs::get(opCtx.get()),
                                      AcquisitionPrerequisites::kRead},
                                  MODE_IS);
            ASSERT_TRUE(coll.exists());
            ASSERT_EQ(coll.uuid(), doc.getSourceUUID());
        }

        donor->abort(false);
        ASSERT_OK(donor->getCompletionFuture().getNoThrow());
        checkStateDocumentRemoved(opCtx.get());

        {
            const auto coll =
                acquireCollection(opCtx.get(),
                                  CollectionAcquisitionRequest{
                                      doc.getSourceNss(),
                                      PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                      repl::ReadConcernArgs::get(opCtx.get()),
                                      AcquisitionPrerequisites::kRead},
                                  MODE_IS);
            ASSERT_TRUE(coll.exists());
            ASSERT_EQ(coll.uuid(), doc.getSourceUUID());
        }
    }
}

TEST_F(ReshardingDonorServiceTest, TruncatesXLErrorOnDonorDocument) {
    for (auto& testOptions : makeAllTestOptions()) {
        LOGV2(5568601,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        std::string xlErrMsg(6000, 'x');
        FailPointEnableBlock failpoint("reshardingDonorFailsAfterTransitionToDonatingOplogEntries",
                                       BSON("errmsg" << xlErrMsg));

        auto doc = makeStateDocument(testOptions);
        auto opCtx = makeOperationContext();
        DonorStateMachine::insertStateDocument(opCtx.get(), doc);
        auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartChangeStreamsMonitor(opCtx.get(), *donor, doc);
        notifyRecipientsDoneCloning(opCtx.get(), *donor, doc);

        auto localTransitionToErrorFuture = donor->awaitInBlockingWritesOrError();
        ASSERT_OK(localTransitionToErrorFuture.getNoThrow());

        // The donor still waits for the abort decision from the coordinator despite it having
        // errored locally. It is therefore safe to check its local state document until
        // DonorStateMachine::abort() is called.
        {
            auto persistedDonorDocument =
                getPersistedDonorDocument(opCtx.get(), doc.getReshardingUUID());
            ASSERT_EQ(persistedDonorDocument.getMutableState().getState(), DonorStateEnum::kError);
            auto persistedAbortReasonBSON =
                persistedDonorDocument.getMutableState().getAbortReason();
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

        donor->abort(false);
        ASSERT_OK(donor->getCompletionFuture().getNoThrow());
        checkStateDocumentRemoved(opCtx.get());
    }
}

TEST_F(ReshardingDonorServiceTest, RestoreMetricsOnKBlockingWrites) {
    auto kDoneState = DonorStateEnum::kDone;
    PauseDuringStateTransitions stateTransitionsGuard{controller(), kDoneState};
    auto opCtx = makeOperationContext();
    // Only test with verification disabled since this test manually writes to the state doc to
    // force the donor to enter the "blocking-writes" state. The change streams monitor used for
    // verification is expected to get stuck waiting 'ReshardBlockingWrites' event.
    auto doc = makeStateDocument({.isAlsoRecipient = false, .performVerification = false});

    auto makeDonorCtx = [&]() {
        DonorShardContext donorCtx;
        donorCtx.setState(DonorStateEnum::kBlockingWrites);
        donorCtx.setMinFetchTimestamp(Timestamp(Date_t::now()));
        donorCtx.setBytesToClone(1);
        donorCtx.setDocumentsToClone(1);
        return donorCtx;
    };
    doc.setMutableState(makeDonorCtx());

    auto timeNow = getServiceContext()->getFastClockSource()->now();
    const auto opTimeDurationSecs = 60;
    auto commonMetadata = doc.getCommonReshardingMetadata();
    commonMetadata.setStartTime(timeNow - Seconds(opTimeDurationSecs));
    doc.setCommonReshardingMetadata(std::move(commonMetadata));

    createSourceCollection(opCtx.get(), doc);
    DonorStateMachine::insertStateDocument(opCtx.get(), doc);

    // This acquires the critical section required by resharding donor machine when it is in
    // kBlockingWrites.
    ShardingRecoveryService::get(opCtx.get())
        ->acquireRecoverableCriticalSectionBlockWrites(
            opCtx.get(),
            doc.getSourceNss(),
            BSON("command" << "resharding_donor"
                           << "collection" << doc.getSourceNss().toString_forTest()),
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

    auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());
    notifyReshardingCommitting(opCtx.get(), *donor, doc);
    stateTransitionsGuard.wait(kDoneState);

    auto currOp =
        donor
            ->reportForCurrentOp(MongoProcessInterface::CurrentOpConnectionsMode::kExcludeIdle,
                                 MongoProcessInterface::CurrentOpSessionsMode::kExcludeIdle)
            .value();
    ASSERT_EQ(currOp.getStringField("donorState"),
              DonorState_serializer(DonorStateEnum::kBlockingWrites));
    ASSERT_GTE(currOp.getField("totalOperationTimeElapsedSecs").Long(), opTimeDurationSecs);

    stateTransitionsGuard.unset(kDoneState);
    ASSERT_OK(donor->getCompletionFuture().getNoThrow());
}

TEST_F(ReshardingDonorServiceTest, AbortWhileChangeStreamsMonitorInProgress) {
    auto opCtx = makeOperationContext();
    // This test manually writes to the state doc to force the donor to enter the "blocking-writes"
    // state. The change streams monitor used for verification is expected to get stuck waiting
    // 'ReshardBlockingWrites' event until the resharding operation is aborted.
    auto doc = makeStateDocument({.isAlsoRecipient = false, .performVerification = true});

    auto makeDonorCtx = [&]() {
        DonorShardContext donorCtx;
        donorCtx.setState(DonorStateEnum::kBlockingWrites);
        donorCtx.setMinFetchTimestamp(Timestamp(Date_t::now()));
        donorCtx.setBytesToClone(1);
        donorCtx.setDocumentsToClone(1);
        return donorCtx;
    };
    doc.setMutableState(makeDonorCtx());

    auto timeNow = getServiceContext()->getFastClockSource()->now();
    const auto opTimeDurationSecs = 60;
    auto commonMetadata = doc.getCommonReshardingMetadata();
    commonMetadata.setStartTime(timeNow - Seconds(opTimeDurationSecs));
    doc.setCommonReshardingMetadata(std::move(commonMetadata));

    createSourceCollection(opCtx.get(), doc);
    auto makeChangeStreamsMonitorCtx = [&]() {
        ChangeStreamsMonitorContext changeStreamsMonitorCtx(cloneTimestamp, 0 /* documentsDelta */);
        return changeStreamsMonitorCtx;
    };
    doc.setChangeStreamsMonitor(makeChangeStreamsMonitorCtx());

    DonorStateMachine::insertStateDocument(opCtx.get(), doc);

    // This acquires the critical section required by resharding donor machine when it is in
    // kBlockingWrites.
    ShardingRecoveryService::get(opCtx.get())
        ->acquireRecoverableCriticalSectionBlockWrites(
            opCtx.get(),
            doc.getSourceNss(),
            BSON("command" << "resharding_donor"
                           << "collection" << doc.getSourceNss().toString_forTest()),
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

    auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());
    ASSERT_OK(donor->awaitChangeStreamsMonitorStarted().getNoThrow());
    donor->abort(false);
    auto status = donor->awaitChangeStreamsMonitorCompleted().getNoThrow();
    ASSERT((status == ErrorCodes::CallbackCanceled) || (status == ErrorCodes::Interrupted));

    ASSERT_OK(donor->getCompletionFuture().getNoThrow());
}

TEST_F(ReshardingDonorServiceTest, AbortAfterStepUpWithAbortReasonFromCoordinator) {
    repl::primaryOnlyServiceTestStepUpWaitForRebuildComplete.setMode(FailPoint::alwaysOn);
    const auto abortErrMsg = "Recieved abort from the resharding coordinator";

    for (auto& testOptions : makeAllTestOptions()) {
        LOGV2(8743302,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        auto removeDonorDocFailpoint = globalFailPointRegistry().find("removeDonorDocFailpoint");
        auto timesEnteredFailPoint = removeDonorDocFailpoint->setMode(FailPoint::alwaysOn);

        auto doc = makeStateDocument(testOptions);
        auto instanceId =
            BSON(ReshardingDonorDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();
        createSourceCollection(opCtx.get(), doc);
        if (testOptions.isAlsoRecipient) {
            createTemporaryReshardingCollection(opCtx.get(), doc);
        }

        DonorStateMachine::insertStateDocument(opCtx.get(), doc);
        auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        donor->abort(false);
        removeDonorDocFailpoint->waitForTimesEntered(timesEnteredFailPoint + 1);

        // Ensure the node is aborting with abortReason from coordinator.
        {
            auto persistedDonorDocument =
                getPersistedDonorDocument(opCtx.get(), doc.getReshardingUUID());
            auto state = persistedDonorDocument.getMutableState().getState();
            ASSERT_EQ(state, DonorStateEnum::kDone);

            auto abortReason = persistedDonorDocument.getMutableState().getAbortReason();
            ASSERT(abortReason);
            ASSERT_EQ(abortReason->getIntField("code"), ErrorCodes::ReshardCollectionAborted);
            ASSERT_EQ(abortReason->getStringField("errmsg"), abortErrMsg);
        }

        stepDown();
        ASSERT_EQ(donor->getCompletionFuture().getNoThrow(),
                  ErrorCodes::InterruptedDueToReplStateChange);
        donor.reset();

        stepUp(opCtx.get());
        removeDonorDocFailpoint->waitForTimesEntered(timesEnteredFailPoint + 2);

        auto [maybeDonor, isPausedOrShutdown] =
            DonorStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(maybeDonor);
        ASSERT_FALSE(isPausedOrShutdown);
        donor = *maybeDonor;

        removeDonorDocFailpoint->setMode(FailPoint::off);
        ASSERT_OK(donor->getCompletionFuture().getNoThrow());
        checkStateDocumentRemoved(opCtx.get());
    }
}

TEST_F(ReshardingDonorServiceTest, FailoverAfterDonorErrorsPriorToObtainingTimestamp) {
    for (auto& testOptions : makeAllTestOptions()) {
        LOGV2(8743303,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        std::string errMsg("Simulating an unrecoverable error for testing");
        FailPointEnableBlock failpoint("reshardingDonorFailsBeforeObtainingTimestamp",
                                       BSON("errmsg" << errMsg));

        auto doc = makeStateDocument(testOptions);
        auto instanceId =
            BSON(ReshardingDonorDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();
        DonorStateMachine::insertStateDocument(opCtx.get(), doc);
        auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        auto localTransitionToErrorFuture = donor->awaitInBlockingWritesOrError();
        ASSERT_OK(localTransitionToErrorFuture.getNoThrow());

        stepDown();
        ASSERT_EQ(donor->getCompletionFuture().getNoThrow(),
                  ErrorCodes::InterruptedDueToReplStateChange);
        donor.reset();

        stepUp(opCtx.get());
        auto [maybeDonor, isPausedOrShutdown] =
            DonorStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(maybeDonor);
        ASSERT_FALSE(isPausedOrShutdown);
        donor = *maybeDonor;

        {
            auto persistedDonorDocument =
                getPersistedDonorDocument(opCtx.get(), doc.getReshardingUUID());
            auto state = persistedDonorDocument.getMutableState().getState();
            ASSERT_EQ(state, DonorStateEnum::kError);

            auto abortReason = persistedDonorDocument.getMutableState().getAbortReason();
            ASSERT(abortReason);
            ASSERT_EQ(abortReason->getIntField("code"), ErrorCodes::InternalError);
            ASSERT_EQ(abortReason->getStringField("errmsg"),
                      "Simulating an unrecoverable error for testing");
        }

        donor->abort(false);
        ASSERT_OK(donor->getCompletionFuture().getNoThrow());
    }
}

TEST_F(ReshardingDonorServiceTest, UnrecoverableErrorDuringPreparingToDonate) {
    externalState()->throwUnrecoverableErrorIn(DonorStateEnum::kPreparingToDonate,
                                               kRefreshCollectionPlacementInfo);

    for (auto& test : makeAllTestOptions()) {
        LOGV2(10494604,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = test);

        runUnrecoverableErrorTest(test, DonorStateEnum::kPreparingToDonate);
    }
}

// TODO (SERVER-108852): Enable this test once the resharding donor is able to handle
// errors from the resharding change streams monitor.
// TEST_F(ReshardingDonorServiceTest, UnrecoverableErrorDuringDonatingInitialData) {
//     for (auto& test :
//          std::vector<TestOptions>{{.isAlsoRecipient = false}, {.isAlsoRecipient = true}}) {
//         LOGV2(10885200,
//               "Running case",
//               "test"_attr = unittest::getTestName(),
//               "testOptions"_attr = test);

//         FailPointEnableBlock
//         failpoint("reshardingDonorFailsUpdatingChangeStreamsMonitorProgress");

//         runUnrecoverableErrorTest(test, DonorStateEnum::kDonatingInitialData);
//     }
// }

TEST_F(ReshardingDonorServiceTest, UnrecoverableErrorDuringPreparingToBlockWrites) {
    externalState()->throwUnrecoverableErrorIn(DonorStateEnum::kPreparingToBlockWrites,
                                               kAbortUnpreparedTransactionIfNecessary);

    for (auto& test : makeAllTestOptions()) {
        LOGV2(10494605,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = test);

        runUnrecoverableErrorTest(test, DonorStateEnum::kPreparingToBlockWrites);
    }
}

MONGO_FAIL_POINT_DEFINE(failFinishOpWithWCE);

class ExternalStateForTestWCEOnRefresh : public ExternalStateForTest {
public:
    void refreshCollectionPlacementInfo(OperationContext* opCtx,
                                        const NamespaceString& sourceNss) override {
        if (MONGO_unlikely(failFinishOpWithWCE.shouldFail())) {
            failFinishOpWithWCE.pauseWhileSet(opCtx);
            uasserted(ErrorCodes::WriteConcernTimeout, "mock WCE");
        }

        uassertStatusOK(Status::OK());
    }
};

class ReshardingDonorServiceWithWCEForTest : public ReshardingDonorServiceForTest {
public:
    explicit ReshardingDonorServiceWithWCEForTest(ServiceContext* serviceContext)
        : ReshardingDonorServiceForTest(serviceContext), _serviceContext(serviceContext) {}

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialState) override {
        return std::make_shared<DonorStateMachine>(
            this,
            ReshardingDonorDocument::parse(initialState,
                                           IDLParserContext{"ReshardingDonorServiceForTest"}),
            std::make_unique<ExternalStateForTestWCEOnRefresh>(),
            _serviceContext);
    }

private:
    ServiceContext* _serviceContext;
};

class ReshardingDonorServiceTestWithWCE : public ReshardingDonorServiceTest {
public:
    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<ReshardingDonorServiceWithWCEForTest>(serviceContext);
    }

    void setUp() override {
        ReshardingDonorServiceTest::setUp();
    }
};

TEST_F(ReshardingDonorServiceTestWithWCE,
       RetryOnWCEAfterCriticalSectionDoesNotResetCriticalSectionTime) {
    // No need to test this with all options
    auto testOptions = makeAllTestOptions()[0];

    auto doc = makeStateDocument(testOptions);
    auto opCtx = makeOperationContext();

    createSourceCollection(opCtx.get(), doc);

    DonorStateMachine::insertStateDocument(opCtx.get(), doc);

    auto donor = DonorStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

    notifyToStartChangeStreamsMonitor(opCtx.get(), *donor, doc);
    notifyRecipientsDoneCloning(opCtx.get(), *donor, doc);
    notifyToStartBlockingWrites(opCtx.get(), *donor, doc);
    awaitDonorState(opCtx.get(), doc.getReshardingUUID(), DonorStateEnum::kBlockingWrites);

    // At this point we've entered the critical section. Turn on the failpoint to force a
    // WriteConcernError on the refresh that occurs after transitioning to "done".
    auto timesEntered = failFinishOpWithWCE.setMode(FailPoint::alwaysOn);

    // We'll compare the critical section elapsed time to ensure that the end time for the critical
    // section will not be reset on the retry due to the WriteConcernError. Sleep for 1 second now
    // to ensure the critical section elapsed time is greater than 0 (since nothing is really
    // happening in this unit test this would otherwise happen nearly instantaneously).
    sleepsecs(1);

    awaitChangeStreamsMonitorCompleted(opCtx.get(), *donor, doc);
    notifyReshardingCommitting(opCtx.get(), *donor, doc);
    awaitDonorState(opCtx.get(), doc.getReshardingUUID(), DonorStateEnum::kDone);

    // Wait until we're in the refresh function to ensure we've set the critical section end time.
    // Then, run currentOp and get the elapsed time.
    failFinishOpWithWCE.waitForTimesEntered(timesEntered + 1);
    auto currOp =
        donor->reportForCurrentOp(MongoProcessInterface::CurrentOpConnectionsMode::kExcludeIdle,
                                  MongoProcessInterface::CurrentOpSessionsMode::kExcludeIdle);
    auto elapsedCritSecTime = currOp->getIntField("totalCriticalSectionTimeElapsedSecs");

    // Sleep for 1 more second before turning off the failpoint and throwing the WriteConcernError.
    // This will allow us to assert that despite the extra time (the faked 1 second) that the retry
    // causes, that the critical section elapsed time remains the same.
    sleepsecs(1);
    failFinishOpWithWCE.setMode(FailPoint::off);

    // Assert the operation successfully completes despite the WriteConcernError.
    ASSERT_OK(donor->getCompletionFuture().getNoThrow());
    checkStateDocumentRemoved(opCtx.get());

    auto currOpAfterFinish =
        donor->reportForCurrentOp(MongoProcessInterface::CurrentOpConnectionsMode::kExcludeIdle,
                                  MongoProcessInterface::CurrentOpSessionsMode::kExcludeIdle);
    auto elapsedCritSecTimeAfterFinish =
        currOpAfterFinish->getIntField("totalCriticalSectionTimeElapsedSecs");

    // Assert the critical section elapsed time is unchanged even after the retry.
    ASSERT_EQ(elapsedCritSecTime, elapsedCritSecTimeAfterFinish);
}

}  // namespace
}  // namespace mongo
