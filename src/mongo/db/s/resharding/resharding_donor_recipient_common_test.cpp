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

#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

using namespace fmt::literals;

/**
 * This test fixture does not create any resharding POSs and should be preferred to
 * `ReshardingDonorRecipientCommonTest` when they are not required.
 */
class ReshardingDonorRecipientCommonInternalsTest : public ShardServerTestFixture {
public:
    const UUID kExistingUUID = UUID::gen();
    const Timestamp kExistingTimestamp = Timestamp(10, 5);
    const NamespaceString kOriginalNss = NamespaceString("db", "foo");

    const NamespaceString kTemporaryReshardingNss =
        resharding::constructTemporaryReshardingNss("db", kExistingUUID);
    const std::string kOriginalShardKey = "oldKey";
    const BSONObj kOriginalShardKeyPattern = BSON(kOriginalShardKey << 1);
    const std::string kReshardingKey = "newKey";
    const BSONObj kReshardingKeyPattern = BSON(kReshardingKey << 1);
    const OID kOriginalEpoch = OID::gen();
    const OID kReshardingEpoch = OID::gen();
    const UUID kReshardingUUID = UUID::gen();
    const Timestamp kReshardingTimestamp = Timestamp(kExistingTimestamp.getSecs() + 1, 0);

    const DonorShardFetchTimestamp kThisShard =
        makeDonorShardFetchTimestamp(ShardId("shardOne"), Timestamp(10, 0));
    const DonorShardFetchTimestamp kOtherShard =
        makeDonorShardFetchTimestamp(ShardId("shardTwo"), Timestamp(20, 0));

    const std::vector<DonorShardFetchTimestamp> kShards = {kThisShard, kOtherShard};

    const Timestamp kCloneTimestamp = Timestamp(20, 0);

protected:
    CollectionMetadata makeShardedMetadataForOriginalCollection(
        OperationContext* opCtx, const ShardId& shardThatChunkExistsOn) {
        return makeShardedMetadata(opCtx,
                                   kOriginalNss,
                                   kOriginalShardKey,
                                   kOriginalShardKeyPattern,
                                   kExistingUUID,
                                   kExistingTimestamp,
                                   kOriginalEpoch,
                                   shardThatChunkExistsOn);
    }

    CollectionMetadata makeShardedMetadataForTemporaryReshardingCollection(
        OperationContext* opCtx, const ShardId& shardThatChunkExistsOn) {
        return makeShardedMetadata(opCtx,
                                   kTemporaryReshardingNss,
                                   kReshardingKey,
                                   kReshardingKeyPattern,
                                   kReshardingUUID,
                                   kReshardingTimestamp,
                                   kReshardingEpoch,
                                   shardThatChunkExistsOn);
    }

    CollectionMetadata makeShardedMetadata(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const std::string& shardKey,
                                           const BSONObj& shardKeyPattern,
                                           const UUID& uuid,
                                           const Timestamp& timestamp,
                                           const OID& epoch,
                                           const ShardId& shardThatChunkExistsOn) {
        auto range = ChunkRange(BSON(shardKey << MINKEY), BSON(shardKey << MAXKEY));
        auto chunk = ChunkType(uuid,
                               std::move(range),
                               ChunkVersion({epoch, timestamp}, {1, 0}),
                               shardThatChunkExistsOn);
        ChunkManager cm(kThisShard.getShardId(),
                        DatabaseVersion(uuid, timestamp),
                        makeStandaloneRoutingTableHistory(
                            RoutingTableHistory::makeNew(nss,
                                                         uuid,
                                                         shardKeyPattern,
                                                         nullptr,
                                                         false,
                                                         epoch,
                                                         timestamp,
                                                         boost::none /* timeseriesFields */,
                                                         boost::none,
                                                         boost::none /* chunkSizeBytes */,
                                                         true,
                                                         {std::move(chunk)})),
                        boost::none);

        return CollectionMetadata(std::move(cm), kThisShard.getShardId());
    }

    ReshardingDonorDocument makeDonorStateDoc(NamespaceString sourceNss,
                                              NamespaceString tempReshardingNss,
                                              BSONObj reshardingKey,
                                              std::vector<mongo::ShardId> recipientShards) {
        DonorShardContext donorCtx;
        donorCtx.setState(DonorStateEnum::kPreparingToDonate);

        ReshardingDonorDocument doc(std::move(donorCtx), recipientShards);

        auto sourceUUID = UUID::gen();
        auto commonMetadata = CommonReshardingMetadata(
            UUID::gen(), sourceNss, sourceUUID, tempReshardingNss, reshardingKey);

        doc.setCommonReshardingMetadata(std::move(commonMetadata));
        return doc;
    }

    ReshardingRecipientDocument makeRecipientStateDoc() {
        RecipientShardContext recipCtx;
        recipCtx.setState(RecipientStateEnum::kCloning);

        ReshardingRecipientDocument doc(
            std::move(recipCtx), {kThisShard.getShardId(), kOtherShard.getShardId()}, 1000);

        NamespaceString sourceNss = kOriginalNss;
        auto sourceUUID = UUID::gen();
        auto commonMetadata = CommonReshardingMetadata(
            UUID::gen(), sourceNss, sourceUUID, kTemporaryReshardingNss, kReshardingKeyPattern);

        doc.setCommonReshardingMetadata(std::move(commonMetadata));

        // A document in the cloning state requires a clone timestamp.
        doc.setCloneTimestamp(kCloneTimestamp);
        return doc;
    }

    ReshardingFields createCommonReshardingFields(const UUID& reshardingUUID,
                                                  CoordinatorStateEnum state) {
        auto fields = ReshardingFields(reshardingUUID);
        fields.setState(state);
        return fields;
    };

    void appendDonorFieldsToReshardingFields(ReshardingFields& fields,
                                             const BSONObj& reshardingKey) {
        std::vector<ShardId> donorShardIds;
        for (const auto& shard : kShards) {
            donorShardIds.emplace_back(shard.getShardId());
        }

        fields.setDonorFields(
            TypeCollectionDonorFields(kTemporaryReshardingNss, reshardingKey, donorShardIds));
    }

    void appendRecipientFieldsToReshardingFields(
        ReshardingFields& fields,
        const std::vector<DonorShardFetchTimestamp> donorShards,
        const UUID& existingUUID,
        const NamespaceString& originalNss,
        const boost::optional<Timestamp>& cloneTimestamp = boost::none) {
        auto recipientFields =
            TypeCollectionRecipientFields(donorShards, existingUUID, originalNss, 5000);
        resharding::emplaceCloneTimestampIfExists(recipientFields, cloneTimestamp);
        fields.setRecipientFields(std::move(recipientFields));
    }

    template <class ReshardingDocument>
    void assertCommonDocFieldsMatchReshardingFields(const NamespaceString& nss,
                                                    const UUID& reshardingUUID,
                                                    const UUID& existingUUID,
                                                    const BSONObj& reshardingKey,
                                                    const ReshardingDocument& reshardingDoc) {
        ASSERT_EQ(reshardingDoc.getReshardingUUID(), reshardingUUID);
        ASSERT_EQ(reshardingDoc.getSourceNss(), nss);
        ASSERT_EQ(reshardingDoc.getSourceUUID(), existingUUID);
        ASSERT_BSONOBJ_EQ(reshardingDoc.getReshardingKey().toBSON(), reshardingKey);
    }

    void assertDonorDocMatchesReshardingFields(const NamespaceString& nss,
                                               const UUID& existingUUID,
                                               const ReshardingFields& reshardingFields,
                                               const ReshardingDonorDocument& donorDoc) {
        assertCommonDocFieldsMatchReshardingFields<ReshardingDonorDocument>(
            nss,
            reshardingFields.getReshardingUUID(),
            existingUUID,
            reshardingFields.getDonorFields()->getReshardingKey().toBSON(),
            donorDoc);
        ASSERT(donorDoc.getMutableState().getState() == DonorStateEnum::kPreparingToDonate);
        ASSERT(donorDoc.getMutableState().getMinFetchTimestamp() == boost::none);
    }

    void assertRecipientDocMatchesReshardingFields(
        const CollectionMetadata& metadata,
        const ReshardingFields& reshardingFields,
        const ReshardingRecipientDocument& recipientDoc) {
        assertCommonDocFieldsMatchReshardingFields<ReshardingRecipientDocument>(
            reshardingFields.getRecipientFields()->getSourceNss(),
            reshardingFields.getReshardingUUID(),
            reshardingFields.getRecipientFields()->getSourceUUID(),
            metadata.getShardKeyPattern().toBSON(),
            recipientDoc);

        ASSERT(recipientDoc.getMutableState().getState() ==
               RecipientStateEnum::kAwaitingFetchTimestamp);
        ASSERT(!recipientDoc.getCloneTimestamp());

        const auto donorShards = reshardingFields.getRecipientFields()->getDonorShards();
        std::map<ShardId, DonorShardFetchTimestamp> donorShardMap;
        for (const auto& donor : donorShards) {
            donorShardMap.emplace(donor.getShardId(), donor);
        }

        for (const auto& donorShardFromRecipientDoc : recipientDoc.getDonorShards()) {
            auto donorIter = donorShardMap.find(donorShardFromRecipientDoc.getShardId());
            ASSERT(donorIter != donorShardMap.end());
            ASSERT_EQ(donorIter->second.getMinFetchTimestamp().has_value(),
                      donorShardFromRecipientDoc.getMinFetchTimestamp().has_value());

            if (donorIter->second.getMinFetchTimestamp()) {
                ASSERT_EQ(*donorIter->second.getMinFetchTimestamp(),
                          *donorShardFromRecipientDoc.getMinFetchTimestamp());
            }

            donorShardMap.erase(donorShardFromRecipientDoc.getShardId());
        }

        ASSERT(donorShardMap.empty());
    }

    void addFilteringMetadata(OperationContext* opCtx, NamespaceString sourceNss, ShardId shardId) {
        AutoGetCollection autoColl(opCtx, sourceNss, LockMode::MODE_IS);
        const auto metadata{makeShardedMetadataForOriginalCollection(opCtx, shardId)};
        ScopedSetShardRole scopedSetShardRole{
            opCtx,
            sourceNss,
            ShardVersion(
                metadata.getShardVersion(),
                CollectionIndexes(metadata.getShardVersion(), boost::none)) /* shardVersion */,
            boost::none /* databaseVersion */};

        auto csr = CollectionShardingRuntime::get(opCtx, sourceNss);
        csr->setFilteringMetadata(opCtx, metadata);
        ASSERT(csr->getCurrentMetadataIfKnown());
    }

private:
    DonorShardFetchTimestamp makeDonorShardFetchTimestamp(
        ShardId shardId, boost::optional<Timestamp> fetchTimestamp) {
        DonorShardFetchTimestamp donorFetchTimestamp(shardId);
        donorFetchTimestamp.setMinFetchTimestamp(fetchTimestamp);
        return donorFetchTimestamp;
    }
};

/**
 * This fixture starts with the above internals test and also creates (notably) the resharding donor
 * and recipient POSs.
 */
class ReshardingDonorRecipientCommonTest : public ReshardingDonorRecipientCommonInternalsTest {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();

        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());

        _primaryOnlyServiceRegistry = repl::PrimaryOnlyServiceRegistry::get(getServiceContext());

        std::unique_ptr<ReshardingDonorService> donorService =
            std::make_unique<ReshardingDonorService>(getServiceContext());
        _primaryOnlyServiceRegistry->registerService(std::move(donorService));

        std::unique_ptr<ReshardingRecipientService> recipientService =
            std::make_unique<ReshardingRecipientService>(getServiceContext());
        _primaryOnlyServiceRegistry->registerService(std::move(recipientService));
        _primaryOnlyServiceRegistry->onStartup(operationContext());

        stepUp();
    }

    void tearDown() override {
        WaitForMajorityService::get(getServiceContext()).shutDown();

        Grid::get(operationContext())->getExecutorPool()->shutdownAndJoin();

        _primaryOnlyServiceRegistry->onShutdown();

        Grid::get(operationContext())->clearForUnitTests();

        ShardServerTestFixture::tearDown();
    }

    void stepUp() {
        auto replCoord = repl::ReplicationCoordinator::get(getServiceContext());

        // Advance term
        _term++;

        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        ASSERT_OK(replCoord->updateTerm(operationContext(), _term));

        WriteUnitOfWork wuow{operationContext()};
        replCoord->setMyLastAppliedOpTimeAndWallTime(repl::OpTimeAndWallTime(
            repl::OpTime(repl::getNextOpTime(operationContext()).getTimestamp(), _term), Date_t()));
        wuow.commit();

        _primaryOnlyServiceRegistry->onStepUpComplete(operationContext(), _term);
    }

protected:
    repl::PrimaryOnlyServiceRegistry* _primaryOnlyServiceRegistry;
    long long _term = 0;
};

TEST_F(ReshardingDonorRecipientCommonInternalsTest, ConstructDonorDocumentFromReshardingFields) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadataForOriginalCollection(opCtx, kThisShard.getShardId());
    ScopedSetShardRole scopedSetShardRole{
        opCtx,
        kOriginalNss,
        ShardVersion(metadata.getShardVersion(),
                     CollectionIndexes(metadata.getShardVersion(), boost::none)) /* shardVersion */,
        boost::none /* databaseVersion */};

    auto reshardingFields =
        createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kPreparingToDonate);
    appendDonorFieldsToReshardingFields(reshardingFields, kReshardingKeyPattern);

    auto donorDoc = resharding::constructDonorDocumentFromReshardingFields(
        kOriginalNss, metadata, reshardingFields);
    assertDonorDocMatchesReshardingFields(kOriginalNss, kExistingUUID, reshardingFields, donorDoc);
}

TEST_F(ReshardingDonorRecipientCommonInternalsTest,
       ConstructRecipientDocumentFromReshardingFields) {
    OperationContext* opCtx = operationContext();
    auto metadata =
        makeShardedMetadataForTemporaryReshardingCollection(opCtx, kThisShard.getShardId());
    ScopedSetShardRole scopedSetShardRole{
        opCtx,
        kTemporaryReshardingNss,
        ShardVersion(metadata.getShardVersion(),
                     CollectionIndexes(metadata.getShardVersion(), boost::none)) /* shardVersion */,
        boost::none /* databaseVersion */};

    auto reshardingFields =
        createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kPreparingToDonate);

    appendRecipientFieldsToReshardingFields(reshardingFields, kShards, kExistingUUID, kOriginalNss);

    auto recipientDoc = resharding::constructRecipientDocumentFromReshardingFields(
        opCtx, kTemporaryReshardingNss, metadata, reshardingFields);
    assertRecipientDocMatchesReshardingFields(metadata, reshardingFields, recipientDoc);
}

TEST_F(ReshardingDonorRecipientCommonTest, CreateDonorServiceInstance) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadataForOriginalCollection(opCtx, kThisShard.getShardId());
    ScopedSetShardRole scopedSetShardRole{
        opCtx,
        kOriginalNss,
        ShardVersion(metadata.getShardVersion(),
                     CollectionIndexes(metadata.getShardVersion(), boost::none)) /* shardVersion */,
        boost::none /* databaseVersion */};

    auto reshardingFields =
        createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kPreparingToDonate);
    appendDonorFieldsToReshardingFields(reshardingFields, kReshardingKeyPattern);

    resharding::processReshardingFieldsForCollection(
        opCtx, kOriginalNss, metadata, reshardingFields);

    auto donorStateMachine =
        resharding::tryGetReshardingStateMachine<ReshardingDonorService,
                                                 ReshardingDonorService::DonorStateMachine,
                                                 ReshardingDonorDocument>(opCtx, kReshardingUUID);

    ASSERT(donorStateMachine != boost::none);

    donorStateMachine.value()->interrupt({ErrorCodes::InternalError, "Shut down for test"});
}

TEST_F(ReshardingDonorRecipientCommonTest, CreateRecipientServiceInstance) {
    OperationContext* opCtx = operationContext();
    auto metadata =
        makeShardedMetadataForTemporaryReshardingCollection(opCtx, kThisShard.getShardId());
    ScopedSetShardRole scopedSetShardRole{
        opCtx,
        kTemporaryReshardingNss,
        ShardVersion(metadata.getShardVersion(),
                     CollectionIndexes(metadata.getShardVersion(), boost::none)) /* shardVersion */,
        boost::none /* databaseVersion */};

    auto reshardingFields =
        createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kPreparingToDonate);
    appendRecipientFieldsToReshardingFields(reshardingFields, kShards, kExistingUUID, kOriginalNss);

    resharding::processReshardingFieldsForCollection(
        opCtx, kTemporaryReshardingNss, metadata, reshardingFields);

    auto recipientStateMachine =
        resharding::tryGetReshardingStateMachine<ReshardingRecipientService,
                                                 ReshardingRecipientService::RecipientStateMachine,
                                                 ReshardingRecipientDocument>(opCtx,
                                                                              kReshardingUUID);

    ASSERT(recipientStateMachine != boost::none);

    recipientStateMachine.value()->interrupt({ErrorCodes::InternalError, "Shut down for test"});
}

TEST_F(ReshardingDonorRecipientCommonTest,
       CreateDonorServiceInstanceWithIncorrectCoordinatorState) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadataForOriginalCollection(opCtx, kThisShard.getShardId());
    ScopedSetShardRole scopedSetShardRole{
        opCtx,
        kOriginalNss,
        ShardVersion(metadata.getShardVersion(),
                     CollectionIndexes(metadata.getShardVersion(), boost::none)) /* shardVersion */,
        boost::none /* databaseVersion */};

    auto reshardingFields =
        createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kCommitting);
    appendDonorFieldsToReshardingFields(reshardingFields, kReshardingKeyPattern);

    auto donorStateMachine =
        resharding::tryGetReshardingStateMachine<ReshardingDonorService,
                                                 ReshardingDonorService::DonorStateMachine,
                                                 ReshardingDonorDocument>(opCtx, kReshardingUUID);
    ASSERT(donorStateMachine == boost::none);
}

TEST_F(ReshardingDonorRecipientCommonTest,
       CreateRecipientServiceInstanceWithIncorrectCoordinatorState) {
    OperationContext* opCtx = operationContext();
    auto metadata =
        makeShardedMetadataForTemporaryReshardingCollection(opCtx, kThisShard.getShardId());
    ScopedSetShardRole scopedSetShardRole{
        opCtx,
        kTemporaryReshardingNss,
        ShardVersion(metadata.getShardVersion(),
                     CollectionIndexes(metadata.getShardVersion(), boost::none)) /* shardVersion */,
        boost::none /* databaseVersion */};

    auto reshardingFields =
        createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kCommitting);
    appendRecipientFieldsToReshardingFields(
        reshardingFields, kShards, kExistingUUID, kOriginalNss, kCloneTimestamp);

    ASSERT_THROWS_CODE(resharding::processReshardingFieldsForCollection(
                           opCtx, kTemporaryReshardingNss, metadata, reshardingFields),
                       DBException,
                       5274202);

    auto recipientStateMachine =
        resharding::tryGetReshardingStateMachine<ReshardingRecipientService,
                                                 ReshardingRecipientService::RecipientStateMachine,
                                                 ReshardingRecipientDocument>(opCtx,
                                                                              kReshardingUUID);

    ASSERT(recipientStateMachine == boost::none);
}

TEST_F(ReshardingDonorRecipientCommonTest, ProcessDonorFieldsWhenShardDoesntOwnAnyChunks) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadataForOriginalCollection(opCtx, kOtherShard.getShardId());
    ScopedSetShardRole scopedSetShardRole{
        opCtx,
        kOriginalNss,
        ShardVersion(metadata.getShardVersion(),
                     CollectionIndexes(metadata.getShardVersion(), boost::none)) /* shardVersion */,
        boost::none /* databaseVersion */};

    auto reshardingFields =
        createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kPreparingToDonate);
    appendDonorFieldsToReshardingFields(reshardingFields, kReshardingKeyPattern);

    resharding::processReshardingFieldsForCollection(
        opCtx, kOriginalNss, metadata, reshardingFields);

    auto donorStateMachine =
        resharding::tryGetReshardingStateMachine<ReshardingDonorService,
                                                 ReshardingDonorService::DonorStateMachine,
                                                 ReshardingDonorDocument>(opCtx, kReshardingUUID);

    ASSERT(donorStateMachine == boost::none);
}

TEST_F(ReshardingDonorRecipientCommonTest, ProcessRecipientFieldsWhenShardDoesntOwnAnyChunks) {
    OperationContext* opCtx = operationContext();
    auto metadata =
        makeShardedMetadataForTemporaryReshardingCollection(opCtx, kOtherShard.getShardId());
    ScopedSetShardRole scopedSetShardRole{
        opCtx,
        kTemporaryReshardingNss,
        ShardVersion(metadata.getShardVersion(),
                     CollectionIndexes(metadata.getShardVersion(), boost::none)) /* shardVersion */,
        boost::none /* databaseVersion */};

    auto reshardingFields =
        createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kPreparingToDonate);
    appendRecipientFieldsToReshardingFields(reshardingFields, kShards, kExistingUUID, kOriginalNss);

    resharding::processReshardingFieldsForCollection(
        opCtx, kTemporaryReshardingNss, metadata, reshardingFields);

    auto recipientStateMachine =
        resharding::tryGetReshardingStateMachine<ReshardingRecipientService,
                                                 ReshardingRecipientService::RecipientStateMachine,
                                                 ReshardingRecipientDocument>(opCtx,
                                                                              kReshardingUUID);

    ASSERT(recipientStateMachine == boost::none);
}

TEST_F(ReshardingDonorRecipientCommonTest, ProcessReshardingFieldsWithoutDonorOrRecipientFields) {
    OperationContext* opCtx = operationContext();
    auto metadata =
        makeShardedMetadataForTemporaryReshardingCollection(opCtx, kThisShard.getShardId());
    ScopedSetShardRole scopedSetShardRole{
        opCtx,
        kTemporaryReshardingNss,
        ShardVersion(metadata.getShardVersion(),
                     CollectionIndexes(metadata.getShardVersion(), boost::none)) /* shardVersion */,
        boost::none /* databaseVersion */};

    auto reshardingFields =
        createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kPreparingToDonate);

    ASSERT_THROWS_CODE(resharding::processReshardingFieldsForCollection(
                           opCtx, kTemporaryReshardingNss, metadata, reshardingFields),
                       DBException,
                       5274201);
}

TEST_F(ReshardingDonorRecipientCommonInternalsTest, ClearReshardingFilteringMetaData) {
    OperationContext* opCtx = operationContext();

    const bool scheduleAsyncRefresh = false;
    auto doSetupFunc = [&] {
        // Clear out the resharding donor/recipient metadata collections.
        for (auto const& nss : {NamespaceString::kDonorReshardingOperationsNamespace,
                                NamespaceString::kRecipientReshardingOperationsNamespace}) {
            dropDatabase(opCtx, nss.dbName()).ignore();
        }

        // Assert the prestate has no filtering metadata.
        for (auto const& nss : {kOriginalNss, kTemporaryReshardingNss}) {
            AutoGetCollection autoColl(opCtx, nss, LockMode::MODE_IS);
            auto csr = CollectionShardingRuntime::get(opCtx, nss);
            ASSERT(csr->getCurrentMetadataIfKnown() == boost::none);
        }

        // Add filtering metadata for the collection being resharded.
        addFilteringMetadata(opCtx, kOriginalNss, kThisShard.getShardId());

        // Add filtering metadata for the temporary resharding namespace.
        addFilteringMetadata(opCtx, kTemporaryReshardingNss, kThisShard.getShardId());

        // Prior to adding a resharding document, assert that attempting to clear filtering does
        // nothing.
        resharding::clearFilteringMetadata(opCtx, scheduleAsyncRefresh);

        for (auto const& nss : {kOriginalNss, kTemporaryReshardingNss}) {
            AutoGetCollection autoColl(opCtx, nss, LockMode::MODE_IS);
            auto csr = CollectionShardingRuntime::get(opCtx, nss);
            ASSERT(csr->getCurrentMetadataIfKnown());
        }
    };

    doSetupFunc();
    // Add a resharding donor document that targets the namespaces involved in resharding.
    ReshardingDonorDocument donorDoc =
        makeDonorStateDoc(kOriginalNss,
                          kTemporaryReshardingNss,
                          kReshardingKeyPattern,
                          {kThisShard.getShardId(), kOtherShard.getShardId()});
    ReshardingDonorService::DonorStateMachine::insertStateDocument(opCtx, donorDoc);

    // Clear the filtering metadata (without scheduling a refresh) and assert the metadata is gone.
    resharding::clearFilteringMetadata(opCtx, scheduleAsyncRefresh);

    for (auto const& nss : {kOriginalNss, kTemporaryReshardingNss}) {
        AutoGetCollection autoColl(opCtx, nss, LockMode::MODE_IS);
        auto csr = CollectionShardingRuntime::get(opCtx, nss);
        ASSERT(csr->getCurrentMetadataIfKnown() == boost::none);
    }

    doSetupFunc();
    // Add a resharding recipient document that targets the namespaces involved in resharding.
    ReshardingRecipientDocument recipDoc = makeRecipientStateDoc();
    ReshardingRecipientService::RecipientStateMachine::insertStateDocument(opCtx, recipDoc);

    // Clear the filtering metadata (without scheduling a refresh) and assert the metadata is gone.
    resharding::clearFilteringMetadata(opCtx, scheduleAsyncRefresh);

    for (auto const& nss : {kOriginalNss, kTemporaryReshardingNss}) {
        AutoGetCollection autoColl(opCtx, nss, LockMode::MODE_IS);
        auto csr = CollectionShardingRuntime::get(opCtx, nss);
        ASSERT(csr->getCurrentMetadataIfKnown() == boost::none);
    }
}

TEST_F(ReshardingDonorRecipientCommonInternalsTest, ClearReshardingFilteringMetaDataForActiveOp) {
    OperationContext* opCtx = operationContext();
    NamespaceString sourceNss1 = NamespaceString("db", "one");
    NamespaceString tempReshardingNss1 =
        resharding::constructTemporaryReshardingNss(sourceNss1.db(), UUID::gen());
    NamespaceString sourceNss2 = NamespaceString("db", "two");
    NamespaceString tempReshardingNss2 =
        resharding::constructTemporaryReshardingNss(sourceNss2.db(), UUID::gen());
    ShardId shardId1 = ShardId{"recipient1"};
    ShardId shardId2 = ShardId{"recipient2"};
    ReshardingDonorDocument doc1 =
        makeDonorStateDoc(sourceNss1, tempReshardingNss1, BSON("newKey1" << 1), {shardId1});
    ReshardingDonorDocument doc2 =
        makeDonorStateDoc(sourceNss2, tempReshardingNss2, BSON("newKey2" << 1), {shardId2});

    ReshardingDonorService::DonorStateMachine::insertStateDocument(opCtx, doc1);
    ReshardingDonorService::DonorStateMachine::insertStateDocument(opCtx, doc2);

    // Add filtering metadata for the collection being resharded.
    addFilteringMetadata(opCtx, sourceNss1, {shardId1});
    addFilteringMetadata(opCtx, sourceNss2, {shardId2});

    // Add filtering metadata for the temporary resharding namespace.
    addFilteringMetadata(opCtx, tempReshardingNss1, {shardId1});
    addFilteringMetadata(opCtx, tempReshardingNss2, {shardId2});

    // Clear the filtering metadata (without scheduling a refresh) for only on single operation
    // related namespaces
    resharding::clearFilteringMetadata(opCtx, {sourceNss1, tempReshardingNss1}, false);

    for (auto const& nss : {sourceNss1, tempReshardingNss1}) {
        AutoGetCollection autoColl(opCtx, nss, LockMode::MODE_IS);
        auto csr = CollectionShardingRuntime::get(opCtx, nss);
        ASSERT(csr->getCurrentMetadataIfKnown() == boost::none);
    }

    // Assert that the filtering metadata is not cleared for other operation
    for (auto const& nss : {sourceNss2, tempReshardingNss2}) {
        AutoGetCollection autoColl(opCtx, nss, LockMode::MODE_IS);
        auto csr = CollectionShardingRuntime::get(opCtx, nss);
        ASSERT(csr->getCurrentMetadataIfKnown() != boost::none);
    }
}

}  // namespace
}  // namespace mongo
