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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/s/resharding/resharding_data_replication.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/s/resharding/resharding_recipient_service_external_state.h"

namespace mongo {
namespace {

class ExternalStateForTest : public ReshardingRecipientService::RecipientStateMachineExternalState {
public:
    ShardId myShardId(ServiceContext* serviceContext) const override {
        return ShardId{"myShardId"};
    }

    void refreshCatalogCache(OperationContext* opCtx, const NamespaceString& nss) override {}

    ChunkManager getShardedCollectionRoutingInfo(OperationContext* opCtx,
                                                 const NamespaceString& nss) override {
        invariant(nss == _sourceNss);

        const OID epoch = OID::gen();
        std::vector<ChunkType> chunks = {ChunkType{
            nss,
            ChunkRange{BSON(_currentShardKey << MINKEY), BSON(_currentShardKey << MAXKEY)},
            ChunkVersion(100, 0, epoch, boost::none /* timestamp */),
            _someDonorId}};

        auto rt = RoutingTableHistory::makeNew(_sourceNss,
                                               _sourceUUID,
                                               BSON(_currentShardKey << 1),
                                               nullptr /* defaultCollator */,
                                               false /* unique */,
                                               std::move(epoch),
                                               boost::none /* timestamp */,
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               true /* allowMigrations */,
                                               chunks);

        return ChunkManager(_someDonorId,
                            DatabaseVersion(UUID::gen()),
                            _makeStandaloneRoutingTableHistory(std::move(rt)),
                            boost::none /* clusterTime */);
    }

    MigrationDestinationManager::CollectionOptionsAndUUID getCollectionOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionUUID& uuid,
        Timestamp afterClusterTime,
        StringData reason) override {
        invariant(nss == _sourceNss);
        return {BSONObj(), uuid};
    }

    MigrationDestinationManager::IndexesAndIdIndex getCollectionIndexes(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionUUID& uuid,
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

private:
    RoutingTableHistoryValueHandle _makeStandaloneRoutingTableHistory(RoutingTableHistory rt) {
        const auto version = rt.getVersion();
        return RoutingTableHistoryValueHandle(
            std::move(rt), ComparableChunkVersion::makeComparableChunkVersion(version));
    }

    const StringData _currentShardKey = "oldKey";

    const NamespaceString _sourceNss{"sourcedb", "sourcecollection"};
    const CollectionUUID _sourceUUID = UUID::gen();

    const ShardId _someDonorId{"myDonorId"};
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
};

class ReshardingRecipientServiceForTest : public ReshardingRecipientService {
public:
    explicit ReshardingRecipientServiceForTest(ServiceContext* serviceContext)
        : ReshardingRecipientService(serviceContext) {}

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialState) override {
        return std::make_shared<RecipientStateMachine>(
            this,
            ReshardingRecipientDocument::parse({"ReshardingRecipientServiceForTest"}, initialState),
            std::make_unique<ExternalStateForTest>(),
            [](auto...) { return std::make_unique<DataReplicationForTest>(); });
    }
};

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
    }

    ReshardingRecipientDocument makeStateDocument() {
        RecipientShardContext recipientCtx;
        recipientCtx.setState(RecipientStateEnum::kAwaitingFetchTimestamp);

        ReshardingRecipientDocument doc(std::move(recipientCtx),
                                        {ShardId{"donor1"}, ShardId{"donor2"}, ShardId{"donor3"}},
                                        durationCount<Milliseconds>(Milliseconds{5}));

        NamespaceString sourceNss("sourcedb", "sourcecollection");
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

    void notifyToStartCloning(OperationContext* opCtx,
                              RecipientStateMachine& recipient,
                              const ReshardingRecipientDocument& recipientDoc) {
        _onReshardingFieldsChanges(opCtx, recipient, recipientDoc, CoordinatorStateEnum::kCloning);
    }

    void notifyReshardingOutcomeDecided(OperationContext* opCtx,
                                        RecipientStateMachine& recipient,
                                        const ReshardingRecipientDocument& recipientDoc,
                                        Status outcome) {
        if (outcome.isOK()) {
            _onReshardingFieldsChanges(
                opCtx, recipient, recipientDoc, CoordinatorStateEnum::kDecisionPersisted);
        } else {
            _onReshardingFieldsChanges(
                opCtx, recipient, recipientDoc, CoordinatorStateEnum::kError, std::move(outcome));
        }
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
                                    CoordinatorStateEnum coordinatorState,
                                    boost::optional<Status> abortReason = boost::none) {
        auto reshardingFields = TypeCollectionReshardingFields{recipientDoc.getReshardingUUID()};
        reshardingFields.setRecipientFields(_makeRecipientFields(recipientDoc));
        reshardingFields.setState(coordinatorState);
        emplaceAbortReasonIfExists(reshardingFields, std::move(abortReason));
        recipient.onReshardingFieldsChanges(opCtx, reshardingFields);
    }
};

TEST_F(ReshardingRecipientServiceTest, CanTransitionThroughEachStateToCompletion) {
    auto doc = makeStateDocument();
    auto opCtx = makeOperationContext();
    RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
    auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

    notifyToStartCloning(opCtx.get(), *recipient, doc);
    notifyReshardingOutcomeDecided(opCtx.get(), *recipient, doc, Status::OK());

    ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
}

}  // namespace
}  // namespace mongo
