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

#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/drop_database.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/db/s/resharding/resharding_donor_service.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

/**
 * This test fixture does not create any resharding POSs and should be preferred to
 * `ReshardingDonorRecipientCommonTest` when they are not required.
 */
class ReshardingDonorRecipientCommonInternalsTest
    : public ShardServerTestFixtureWithCatalogCacheMock {
public:
    const UUID kExistingUUID = UUID::gen();
    const Timestamp kExistingTimestamp = Timestamp(10, 5);
    const NamespaceString kOriginalNss =
        NamespaceString::createNamespaceString_forTest("db", "foo");

    const NamespaceString kTemporaryReshardingNss =
        resharding::constructTemporaryReshardingNss(kOriginalNss, kExistingUUID);
    const std::string kOriginalShardKey = "oldKey";
    const BSONObj kOriginalShardKeyPattern = BSON(kOriginalShardKey << 1);
    const std::string kReshardingKey = "newKey";
    const BSONObj kReshardingKeyPattern = BSON(kReshardingKey << 1);
    const OID kOriginalEpoch = OID::gen();
    const OID kReshardingEpoch = OID::gen();
    const UUID kReshardingUUID = UUID::gen();
    const Timestamp kReshardingTimestamp = Timestamp(kExistingTimestamp.getSecs() + 1, 0);

    const DonorShardFetchTimestamp kThisShard =
        makeDonorShardFetchTimestamp(kMyShardName, Timestamp(10, 0));
    const DonorShardFetchTimestamp kOtherShard =
        makeDonorShardFetchTimestamp(ShardId("otherShardName"), Timestamp(20, 0));

    const std::vector<DonorShardFetchTimestamp> kShards = {kThisShard, kOtherShard};

    const Timestamp kCloneTimestamp = Timestamp(20, 0);

protected:
    CollectionMetadata makeShardedMetadataForOriginalCollection(
        OperationContext* opCtx,
        const ShardId& shardThatChunkExistsOn,
        const boost::optional<ShardId>& primaryShard = boost::none) {
        return makeShardedMetadata(opCtx,
                                   kOriginalNss,
                                   kOriginalShardKey,
                                   kOriginalShardKeyPattern,
                                   kExistingUUID,
                                   kExistingTimestamp,
                                   kOriginalEpoch,
                                   shardThatChunkExistsOn,
                                   primaryShard ? *primaryShard : kThisShard.getShardId());
    }

    CollectionMetadata makeShardedMetadataForTemporaryReshardingCollection(
        OperationContext* opCtx,
        const ShardId& shardThatChunkExistsOn,
        const boost::optional<ShardId>& primaryShard = boost::none) {
        return makeShardedMetadata(opCtx,
                                   kTemporaryReshardingNss,
                                   kReshardingKey,
                                   kReshardingKeyPattern,
                                   kReshardingUUID,
                                   kReshardingTimestamp,
                                   kReshardingEpoch,
                                   shardThatChunkExistsOn,
                                   primaryShard ? *primaryShard : kThisShard.getShardId());
    }

    CollectionMetadata makeShardedMetadata(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const std::string& shardKey,
                                           const BSONObj& shardKeyPattern,
                                           const UUID& uuid,
                                           const Timestamp& timestamp,
                                           const OID& epoch,
                                           const ShardId& shardThatChunkExistsOn,
                                           const ShardId& primaryShard) {
        auto range = ChunkRange(BSON(shardKey << MINKEY), BSON(shardKey << MAXKEY));
        auto chunk = ChunkType(
            uuid, range, ChunkVersion({epoch, timestamp}, {1, 0}), shardThatChunkExistsOn);
        ChunkManager cm(makeStandaloneRoutingTableHistory(
                            RoutingTableHistory::makeNew(nss,
                                                         uuid,
                                                         shardKeyPattern,
                                                         false, /* unsplittable */
                                                         nullptr,
                                                         false,
                                                         epoch,
                                                         timestamp,
                                                         boost::none /* timeseriesFields */,
                                                         boost::none /* reshardingFields */,
                                                         true,
                                                         {std::move(chunk)})),
                        boost::none);
        auto dbVersion = DatabaseVersion(uuid, timestamp);
        getCatalogCacheMock()->setDatabaseReturnValue(
            nss.dbName(),
            CatalogCacheMock::makeDatabaseInfo(nss.dbName(), primaryShard, dbVersion));
        getCatalogCacheMock()->setCollectionReturnValue(
            nss,
            CatalogCacheMock::makeCollectionRoutingInfoSharded(
                nss, primaryShard, dbVersion, shardKeyPattern, {{range, shardThatChunkExistsOn}}));
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
                                                    bool performVerification,
                                                    const ReshardingDocument& reshardingDoc) {
        ASSERT_EQ(reshardingDoc.getReshardingUUID(), reshardingUUID);
        ASSERT_EQ(reshardingDoc.getSourceNss(), nss);
        ASSERT_EQ(reshardingDoc.getSourceUUID(), existingUUID);
        ASSERT_BSONOBJ_EQ(reshardingDoc.getReshardingKey().toBSON(), reshardingKey);
        ASSERT_EQ(reshardingDoc.getPerformVerification(), performVerification);
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
            reshardingFields.getPerformVerification(),
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
            reshardingFields.getPerformVerification(),
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
        const auto dataColl =
            acquireCollection(opCtx,
                              CollectionAcquisitionRequest{sourceNss,
                                                           PlacementConcern::kPretendUnsharded,
                                                           repl::ReadConcernArgs::get(opCtx),
                                                           AcquisitionPrerequisites::kRead},
                              MODE_IS);
        const auto metadata{makeShardedMetadataForOriginalCollection(opCtx, shardId)};
        ScopedSetShardRole scopedSetShardRole{
            opCtx,
            sourceNss,
            ShardVersionFactory::make(metadata) /* shardVersion */,
            boost::none /* databaseVersion */};

        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, sourceNss)
            ->setFilteringMetadata(opCtx, metadata);
    }

private:
    DonorShardFetchTimestamp makeDonorShardFetchTimestamp(
        ShardId shardId, boost::optional<Timestamp> fetchTimestamp) {
        DonorShardFetchTimestamp donorFetchTimestamp(shardId);
        donorFetchTimestamp.setMinFetchTimestamp(fetchTimestamp);
        return donorFetchTimestamp;
    }
};

struct DonorFieldsValidator {
    void validate(ReshardingDonorDocument doc) {
        // 'performVerification' should only be set if it is specified.
        if (performVerification) {
            ASSERT_EQ(doc.getPerformVerification(), *performVerification);
        } else {
            ASSERT_FALSE(doc.getPerformVerification().has_value());
            ASSERT_EQ(doc.getPerformVerification(), false);
        }
    }

    boost::optional<bool> performVerification;
};

struct RecipientFieldsValidator {
    void validate(ReshardingRecipientDocument doc) {
        // 'skipCloningAndApplying' should only be set if it is true.
        if (skipCloningAndApplying) {
            ASSERT_EQ(doc.getSkipCloningAndApplying(), skipCloningAndApplying);
        } else {
            ASSERT_FALSE(doc.getSkipCloningAndApplying().has_value());
            ASSERT_EQ(doc.getSkipCloningAndApplying(), false);
        }
        // 'storeOplogFetcherProgress' should only be set if it is true.
        if (storeOplogFetcherProgress) {
            ASSERT_EQ(doc.getStoreOplogFetcherProgress(), storeOplogFetcherProgress);
        } else {
            ASSERT_FALSE(doc.getStoreOplogFetcherProgress().has_value());
            ASSERT_EQ(doc.getStoreOplogFetcherProgress(), false);
        }
        // 'performVerification' should only be set if it is specified.
        if (performVerification) {
            ASSERT_EQ(doc.getPerformVerification(), *performVerification);
        } else {
            ASSERT_FALSE(doc.getPerformVerification().has_value());
            ASSERT_EQ(doc.getPerformVerification(), false);
        }
    }

    bool skipCloningAndApplying;
    // featureFlagReshardingStoreOplogFetcherProgress defaults to true.
    bool storeOplogFetcherProgress = true;
    boost::optional<bool> performVerification;
};

/**
 * This fixture starts with the above internals test and also creates (notably) the resharding donor
 * and recipient POSs.
 */
class ReshardingDonorRecipientCommonTest : service_context_test::WithSetupTransportLayer,
                                           public ReshardingDonorRecipientCommonInternalsTest {
public:
    void setUp() override {
        ShardServerTestFixtureWithCatalogCacheMock::setUp();

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

        shutdownExecutorPool();

        _primaryOnlyServiceRegistry->onShutdown();

        Grid::get(operationContext())->clearForUnitTests();

        ShardServerTestFixtureWithCatalogCacheMock::tearDown();
    }

    void stepUp() {
        auto replCoord = repl::ReplicationCoordinator::get(getServiceContext());

        // Advance term
        _term++;

        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        ASSERT_OK(replCoord->updateTerm(operationContext(), _term));

        WriteUnitOfWork wuow{operationContext()};
        replCoord->setMyLastAppliedOpTimeAndWallTimeForward(repl::OpTimeAndWallTime(
            repl::OpTime(repl::getNextOpTime(operationContext()).getTimestamp(), _term), Date_t()));
        wuow.commit();

        _primaryOnlyServiceRegistry->onStepUpComplete(operationContext(), _term);
    }

    void testProcessDonorFields(
        const ShardId& shardThatChunkExistsOn,
        const ShardId& primaryShard,
        boost::optional<bool> performVerification,
        bool expectDonorStateMachine,
        boost::optional<DonorFieldsValidator> fieldsValidator = boost::none) {
        ASSERT(!expectDonorStateMachine || fieldsValidator.has_value());
        OperationContext* opCtx = operationContext();

        auto temporaryCollMetadata =
            makeShardedMetadataForOriginalCollection(opCtx, shardThatChunkExistsOn, primaryShard);
        ScopedSetShardRole scopedSetShardRole{
            opCtx,
            kOriginalNss,
            ShardVersionFactory::make(temporaryCollMetadata) /* shardVersion */,
            boost::none /* databaseVersion */};

        auto reshardingFields =
            createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kPreparingToDonate);
        if (performVerification) {
            reshardingFields.setPerformVerification(*performVerification);
        }
        appendDonorFieldsToReshardingFields(reshardingFields, kReshardingKeyPattern);

        resharding::processReshardingFieldsForCollection(
            opCtx, kOriginalNss, temporaryCollMetadata, reshardingFields);

        auto donorStateMachine =
            resharding::tryGetReshardingStateMachine<ReshardingDonorService,
                                                     ReshardingDonorService::DonorStateMachine,
                                                     ReshardingDonorDocument>(opCtx,
                                                                              kReshardingUUID);

        if (expectDonorStateMachine) {
            ASSERT(donorStateMachine != boost::none);
            auto donorDoc = getPersistedDonorDocument(opCtx, kReshardingUUID);
            fieldsValidator->validate(donorDoc);
        } else {
            ASSERT(donorStateMachine == boost::none);
        }
    }

    void testProcessRecipientFields(
        const ShardId& shardThatChunkExistsOn,
        const ShardId& primaryShard,
        boost::optional<bool> performVerification,
        bool expectRecipientStateMachine,
        boost::optional<RecipientFieldsValidator> fieldsValidator = boost::none) {
        ASSERT(!expectRecipientStateMachine || fieldsValidator.has_value());
        OperationContext* opCtx = operationContext();

        auto originalCollMetadata =
            makeShardedMetadataForOriginalCollection(opCtx, shardThatChunkExistsOn, primaryShard);

        auto temporaryCollMetadata = makeShardedMetadataForTemporaryReshardingCollection(
            opCtx, shardThatChunkExistsOn, primaryShard);

        ScopedSetShardRole scopedSetShardRole{
            opCtx,
            kTemporaryReshardingNss,
            ShardVersionFactory::make(temporaryCollMetadata) /* shardVersion */,
            boost::none /* databaseVersion */};

        auto reshardingFields =
            createCommonReshardingFields(kReshardingUUID, CoordinatorStateEnum::kPreparingToDonate);
        if (performVerification) {
            reshardingFields.setPerformVerification(*performVerification);
        }
        appendRecipientFieldsToReshardingFields(
            reshardingFields, kShards, kExistingUUID, kOriginalNss);

        resharding::processReshardingFieldsForCollection(
            opCtx, kTemporaryReshardingNss, temporaryCollMetadata, reshardingFields);

        auto recipientStateMachine = resharding::tryGetReshardingStateMachine<
            ReshardingRecipientService,
            ReshardingRecipientService::RecipientStateMachine,
            ReshardingRecipientDocument>(opCtx, kReshardingUUID);

        if (expectRecipientStateMachine) {
            ASSERT(recipientStateMachine != boost::none);

            // Make the recipient transition to the "cloning" state and check its cloning metrics.
            reshardingFields.setState(mongo::CoordinatorStateEnum::kCloning);
            reshardingFields.getRecipientFields()->setCloneTimestamp(_minFetchTimestamp);
            reshardingFields.getRecipientFields()->setApproxBytesToCopy(_approxBytesToCopy);
            reshardingFields.getRecipientFields()->setApproxDocumentsToCopy(_approxDocumentsToCopy);
            resharding::processReshardingFieldsForCollection(
                opCtx, kTemporaryReshardingNss, temporaryCollMetadata, reshardingFields);

            auto driveCloneNoRefresh =
                resharding::gFeatureFlagReshardingCloneNoRefresh.isEnabledAndIgnoreFCVUnsafe();
            if (driveCloneNoRefresh) {
                auto recipientDoc = getPersistedRecipientDocument(opCtx, kReshardingUUID);
                ASSERT(!recipientDoc.getCloneTimestamp());
            } else {
                bool noChunksToCopy = shardThatChunkExistsOn != kThisShard.getShardId();
                while (true) {
                    auto recipientDoc = getPersistedRecipientDocument(opCtx, kReshardingUUID);
                    fieldsValidator->validate(recipientDoc);
                    if (!recipientDoc.getCloneTimestamp()) {
                        opCtx->sleepFor(Milliseconds{10});
                        continue;
                    }
                    auto metrics = recipientDoc.getMetrics();
                    ASSERT_EQ(*metrics->getApproxBytesToCopy(),
                              noChunksToCopy ? 0 : _approxBytesToCopy);
                    ASSERT_EQ(*metrics->getApproxDocumentsToCopy(),
                              noChunksToCopy ? 0 : _approxDocumentsToCopy);
                    break;
                }
                // Schedule a dummy response to the find command against config.shards from the
                // shard registry to avoid a hang.
                onCommand([&](const executor::RemoteCommandRequest& request) {
                    ASSERT_EQ(request.dbname, DatabaseName::kConfig);
                    auto firstElement = request.cmdObj.firstElement();
                    ASSERT_EQ(firstElement.fieldNameStringData(), "find");
                    ASSERT_EQ(firstElement.str(), "shards");
                    return BSONObj();
                });
            }
        } else {
            ASSERT(recipientStateMachine == boost::none);
        }
    }

protected:
    ReshardingDonorDocument getPersistedDonorDocument(OperationContext* opCtx,
                                                      UUID reshardingUUID) {
        boost::optional<ReshardingDonorDocument> persistedDonorDocument;
        PersistentTaskStore<ReshardingDonorDocument> store(
            NamespaceString::kDonorReshardingOperationsNamespace);
        store.forEach(opCtx,
                      BSON(ReshardingDonorDocument::kReshardingUUIDFieldName << reshardingUUID),
                      [&](const auto& DonorDocument) {
                          persistedDonorDocument.emplace(DonorDocument);
                          return false;
                      });
        ASSERT(persistedDonorDocument);
        return persistedDonorDocument.get();
    }

    ReshardingRecipientDocument getPersistedRecipientDocument(OperationContext* opCtx,
                                                              UUID reshardingUUID) {
        boost::optional<ReshardingRecipientDocument> persistedRecipientDocument;
        PersistentTaskStore<ReshardingRecipientDocument> store(
            NamespaceString::kRecipientReshardingOperationsNamespace);
        store.forEach(opCtx,
                      BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << reshardingUUID),
                      [&](const auto& recipientDocument) {
                          persistedRecipientDocument.emplace(recipientDocument);
                          return false;
                      });
        ASSERT(persistedRecipientDocument);
        return persistedRecipientDocument.get();
    }

    repl::PrimaryOnlyServiceRegistry* _primaryOnlyServiceRegistry;
    long long _term = 0;
    Timestamp _minFetchTimestamp{100, 1};
    long _approxBytesToCopy = 10000;
    long _approxDocumentsToCopy = 100;
};

TEST_F(ReshardingDonorRecipientCommonInternalsTest, PerformVerificationDefaultReshardingFields) {
    ReshardingFields reshardingFields;
    // The default should be false since the absence of this field implies that the cluster might
    // contain nodes that do not support verification.
    ASSERT_EQ(reshardingFields.getPerformVerification(), false);
}

TEST_F(ReshardingDonorRecipientCommonInternalsTest, PerformVerificationDefaultDonorStateDocument) {
    DonorShardContext donorCtx;
    donorCtx.setState(DonorStateEnum::kPreparingToDonate);
    std::vector<ShardId> recipientShards{kThisShard.getShardId(), kOtherShard.getShardId()};
    ReshardingDonorDocument doc(std::move(donorCtx), recipientShards);
    // The default should be false since the absence of this field implies that the cluster might
    // contain nodes that do not support verification.
    ASSERT_EQ(doc.getPerformVerification(), false);
}

TEST_F(ReshardingDonorRecipientCommonInternalsTest,
       PerformVerificationDefaultRecipientStateDocument) {
    RecipientShardContext recipientCtx;
    recipientCtx.setState(RecipientStateEnum::kCloning);
    ReshardingRecipientDocument doc(
        std::move(recipientCtx), {kThisShard.getShardId(), kOtherShard.getShardId()}, 1000);
    // The default should be false since the absence of this field implies that the cluster might
    // contain nodes that do not support verification.
    ASSERT_EQ(doc.getPerformVerification(), false);
}

TEST_F(ReshardingDonorRecipientCommonInternalsTest, ConstructDonorDocumentFromReshardingFields) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadataForOriginalCollection(opCtx, kThisShard.getShardId());
    ScopedSetShardRole scopedSetShardRole{opCtx,
                                          kOriginalNss,
                                          ShardVersionFactory::make(metadata) /* shardVersion */,
                                          boost::none /* databaseVersion */};

    for (bool performVerification : {true, false}) {
        for (bool enableVerification : {true, false}) {
            LOGV2(9849100,
                  "Running case",
                  "test"_attr = unittest::getTestName(),
                  "performVerification"_attr = performVerification,
                  "enableVerification"_attr = enableVerification);

            RAIIServerParameterControllerForTest verificationFeatureFlagController(
                "featureFlagReshardingVerification", enableVerification);

            auto reshardingFields = createCommonReshardingFields(
                kReshardingUUID, CoordinatorStateEnum::kPreparingToDonate);
            reshardingFields.setPerformVerification(performVerification);

            appendDonorFieldsToReshardingFields(reshardingFields, kReshardingKeyPattern);
            if (performVerification && !enableVerification) {
                ASSERT_THROWS_CODE(resharding::constructDonorDocumentFromReshardingFields(
                                       VersionContext::getDecoration(opCtx),
                                       kOriginalNss,
                                       metadata,
                                       reshardingFields),
                                   DBException,
                                   ErrorCodes::InvalidOptions);
                continue;
            }
            auto donorDoc = resharding::constructDonorDocumentFromReshardingFields(
                VersionContext::getDecoration(opCtx), kOriginalNss, metadata, reshardingFields);
            assertDonorDocMatchesReshardingFields(
                kOriginalNss, kExistingUUID, reshardingFields, donorDoc);
        }
    }
}

TEST_F(ReshardingDonorRecipientCommonInternalsTest,
       ConstructRecipientDocumentFromReshardingFields) {
    OperationContext* opCtx = operationContext();
    auto metadata =
        makeShardedMetadataForTemporaryReshardingCollection(opCtx, kThisShard.getShardId());
    ScopedSetShardRole scopedSetShardRole{opCtx,
                                          kTemporaryReshardingNss,
                                          ShardVersionFactory::make(metadata) /* shardVersion */,
                                          boost::none /* databaseVersion */};

    for (bool performVerification : {true, false}) {
        for (bool enableVerification : {true, false}) {
            LOGV2(9849101,
                  "Running case",
                  "test"_attr = unittest::getTestName(),
                  "performVerification"_attr = performVerification,
                  "enableVerification"_attr = enableVerification);

            RAIIServerParameterControllerForTest verificationFeatureFlagController(
                "featureFlagReshardingVerification", enableVerification);

            auto reshardingFields = createCommonReshardingFields(
                kReshardingUUID, CoordinatorStateEnum::kPreparingToDonate);
            reshardingFields.setPerformVerification(performVerification);

            appendRecipientFieldsToReshardingFields(
                reshardingFields, kShards, kExistingUUID, kOriginalNss);
            if (performVerification && !enableVerification) {
                ASSERT_THROWS_CODE(resharding::constructRecipientDocumentFromReshardingFields(
                                       VersionContext::getDecoration(opCtx),
                                       kTemporaryReshardingNss,
                                       metadata,
                                       reshardingFields),
                                   DBException,
                                   ErrorCodes::InvalidOptions);
                continue;
            }
            auto recipientDoc = resharding::constructRecipientDocumentFromReshardingFields(
                VersionContext::getDecoration(opCtx),
                kTemporaryReshardingNss,
                metadata,
                reshardingFields);
            assertRecipientDocMatchesReshardingFields(metadata, reshardingFields, recipientDoc);
        }
    }
}

TEST_F(ReshardingDonorRecipientCommonTest, CreateDonorServiceInstance) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadataForOriginalCollection(opCtx, kThisShard.getShardId());
    ScopedSetShardRole scopedSetShardRole{opCtx,
                                          kOriginalNss,
                                          ShardVersionFactory::make(metadata) /* shardVersion */,
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
    ScopedSetShardRole scopedSetShardRole{opCtx,
                                          kTemporaryReshardingNss,
                                          ShardVersionFactory::make(metadata) /* shardVersion */,
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
    ScopedSetShardRole scopedSetShardRole{opCtx,
                                          kOriginalNss,
                                          ShardVersionFactory::make(metadata) /* shardVersion */,
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
    ScopedSetShardRole scopedSetShardRole{opCtx,
                                          kTemporaryReshardingNss,
                                          ShardVersionFactory::make(metadata) /* shardVersion */,
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

TEST_F(ReshardingDonorRecipientCommonTest,
       ProcessDonorFieldsWhenShardDoesNotOwnAnyChunks_NotPrimaryShard) {
    testProcessDonorFields(kOtherShard.getShardId() /* shardThatChunkExistsOn*/,
                           kOtherShard.getShardId() /* primaryShard */,
                           boost::none /* performVerification */,
                           false /* expectDonorStateMachine */);
}

TEST_F(ReshardingDonorRecipientCommonTest,
       ProcessDonorFieldsWhenShardDoesNotOwnAnyChunks_PrimaryShard) {
    testProcessDonorFields(kOtherShard.getShardId() /* shardThatChunkExistsOn*/,
                           kThisShard.getShardId() /* primaryShard */,
                           boost::none /* performVerification */,
                           false /* expectDonorStateMachine */);
}

TEST_F(ReshardingDonorRecipientCommonTest,
       ProcessDonorFieldsPerformVerificationUnspecified_FeatureFlagEnabled) {
    RAIIServerParameterControllerForTest verificationFeatureFlagController(
        "featureFlagReshardingVerification", true);
    auto performVerification = boost::none;

    testProcessDonorFields(kThisShard.getShardId() /* shardThatChunkExistsOn*/,
                           kOtherShard.getShardId() /* primaryShard */,
                           performVerification,
                           true /* expectDonorStateMachine */,
                           DonorFieldsValidator{});
}

TEST_F(ReshardingDonorRecipientCommonTest,
       ProcessDonorFieldsPerformVerificationUnspecified_FeatureFlagDisabled) {
    RAIIServerParameterControllerForTest verificationFeatureFlagController(
        "featureFlagReshardingVerification", false);
    auto performVerification = boost::none;

    testProcessDonorFields(kThisShard.getShardId() /* shardThatChunkExistsOn*/,
                           kOtherShard.getShardId() /* primaryShard */,
                           performVerification,
                           true /* expectDonorStateMachine */,
                           DonorFieldsValidator{});
}

TEST_F(ReshardingDonorRecipientCommonTest, ProcessDonorFieldsNotPerformVerification) {
    bool performVerification = false;

    testProcessDonorFields(kThisShard.getShardId() /* shardThatChunkExistsOn*/,
                           kOtherShard.getShardId() /* primaryShard */,
                           performVerification,
                           true /* expectDonorStateMachine */,
                           DonorFieldsValidator{.performVerification = performVerification});
}

TEST_F(ReshardingDonorRecipientCommonTest,
       ProcessDonorFieldsPerformVerification_FeatureFlagEnabled) {
    RAIIServerParameterControllerForTest verificationFeatureFlagController(
        "featureFlagReshardingVerification", true);
    bool performVerification = true;

    testProcessDonorFields(kThisShard.getShardId() /* shardThatChunkExistsOn*/,
                           kOtherShard.getShardId() /* primaryShard */,
                           performVerification,
                           true /* expectDonorStateMachine */,
                           DonorFieldsValidator{.performVerification = performVerification});
}

TEST_F(ReshardingDonorRecipientCommonTest,
       ProcessDonorFieldsPerformVerification_FeatureFlagDisabled) {
    RAIIServerParameterControllerForTest verificationFeatureFlagController(
        "featureFlagReshardingVerification", false);
    bool performVerification = true;

    ASSERT_THROWS_CODE(testProcessDonorFields(kThisShard.getShardId() /* shardThatChunkExistsOn*/,
                                              kOtherShard.getShardId() /* primaryShard */,
                                              performVerification,
                                              false /* expectDonorStateMachine */),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(ReshardingDonorRecipientCommonTest,
       ProcessRecipientFieldsWhenShardOwnsChunks_StoreOplogFetcherProgress) {
    // Not set featureFlagReshardingStoreOplogFetcherProgress to verify that it defaults to true.

    testProcessRecipientFields(kThisShard.getShardId() /* shardThatChunkExistsOn*/,
                               kThisShard.getShardId() /* primaryShard */,
                               boost::none /* performVerification */,
                               true /* expectRecipientStateMachine */,
                               RecipientFieldsValidator{.storeOplogFetcherProgress = true});
}

TEST_F(ReshardingDonorRecipientCommonTest,
       ProcessRecipientFieldsWhenShardOwnsChunks_NotStoreOplogFetcherProgress) {
    RAIIServerParameterControllerForTest storeOplogFetcherProgressFeatureFlagController(
        "featureFlagReshardingStoreOplogFetcherProgress", false);

    testProcessRecipientFields(kThisShard.getShardId() /* shardThatChunkExistsOn*/,
                               kThisShard.getShardId() /* primaryShard */,
                               boost::none /* performVerification */,
                               true /* expectRecipientStateMachine */,
                               RecipientFieldsValidator{.storeOplogFetcherProgress = false});
}

TEST_F(ReshardingDonorRecipientCommonTest,
       ProcessRecipientFieldsWhenShardDoesNotOwnAnyChunks_NotPrimaryShard) {
    testProcessRecipientFields(kOtherShard.getShardId() /* shardThatChunkExistsOn*/,
                               kOtherShard.getShardId() /* primaryShard */,
                               boost::none /* performVerification */,
                               false /* expectRecipientStateMachine */);
}

TEST_F(ReshardingDonorRecipientCommonTest,
       ProcessRecipientFieldsWhenShardDoesNotOwnAnyChunks_PrimaryShard_SkipIfApplicable) {
    RAIIServerParameterControllerForTest skipCloningAndApplyingFeatureFlagController(
        "featureFlagReshardingSkipCloningAndApplyingIfApplicable", true);

    testProcessRecipientFields(kOtherShard.getShardId() /* shardThatChunkExistsOn*/,
                               kThisShard.getShardId() /* primaryShard */,
                               boost::none /* performVerification */,
                               true /* expectRecipientStateMachine */,
                               RecipientFieldsValidator{.skipCloningAndApplying = true});
}

TEST_F(ReshardingDonorRecipientCommonTest,
       ProcessRecipientFieldsWhenShardDoesNotOwnAnyChunks_PrimaryShard_NotSkipIfApplicable) {
    RAIIServerParameterControllerForTest skipCloningAndApplyingFeatureFlagController(
        "featureFlagReshardingSkipCloningAndApplyingIfApplicable", false);

    testProcessRecipientFields(kOtherShard.getShardId() /* shardThatChunkExistsOn*/,
                               kThisShard.getShardId() /* primaryShard */,
                               boost::none /* performVerification */,
                               true /* expectRecipientStateMachine */,
                               RecipientFieldsValidator{.skipCloningAndApplying = false});
}

TEST_F(ReshardingDonorRecipientCommonTest,
       ProcessRecipientFieldsPerformVerificationUnspecified_FeatureFlagEnabled) {
    RAIIServerParameterControllerForTest verificationFeatureFlagController(
        "featureFlagReshardingVerification", true);
    boost::optional<bool> performVerification = boost::none;

    testProcessRecipientFields(kThisShard.getShardId() /* shardThatChunkExistsOn*/,
                               kOtherShard.getShardId() /* primaryShard */,
                               performVerification,
                               true /* expectRecipientStateMachine */,
                               RecipientFieldsValidator{});
}

TEST_F(ReshardingDonorRecipientCommonTest,
       ProcessRecipientFieldsPerformVerificationUnspecified_FeatureFlagDisabled) {
    RAIIServerParameterControllerForTest verificationFeatureFlagController(
        "featureFlagReshardingVerification", false);
    auto performVerification = boost::none;

    testProcessRecipientFields(kThisShard.getShardId() /* shardThatChunkExistsOn*/,
                               kOtherShard.getShardId() /* primaryShard */,
                               performVerification,
                               true /* expectRecipientStateMachine */,
                               RecipientFieldsValidator{});
}

TEST_F(ReshardingDonorRecipientCommonTest, ProcessRecipientFieldsNotPerformVerification) {
    bool performVerification = false;

    testProcessRecipientFields(
        kThisShard.getShardId() /* shardThatChunkExistsOn*/,
        kOtherShard.getShardId() /* primaryShard */,
        performVerification,
        true /* expectRecipientStateMachine */,
        RecipientFieldsValidator{.performVerification = performVerification});
}

TEST_F(ReshardingDonorRecipientCommonTest,
       ProcessRecipientFieldsPerformVerification_FeatureFlagEnabled) {
    RAIIServerParameterControllerForTest verificationFeatureFlagController(
        "featureFlagReshardingVerification", true);
    bool performVerification = true;

    testProcessRecipientFields(
        kThisShard.getShardId() /* shardThatChunkExistsOn*/,
        kOtherShard.getShardId() /* primaryShard */,
        performVerification,
        true /* expectRecipientStateMachine */,
        RecipientFieldsValidator{.performVerification = performVerification});
}

TEST_F(ReshardingDonorRecipientCommonTest,
       ProcessRecipientFieldsPerformVerification_FeatureFlagDisabled) {
    RAIIServerParameterControllerForTest verificationFeatureFlagController(
        "featureFlagReshardingVerification", false);
    bool performVerification = true;

    ASSERT_THROWS_CODE(
        testProcessRecipientFields(kThisShard.getShardId() /* shardThatChunkExistsOn*/,
                                   kOtherShard.getShardId() /* primaryShard */,
                                   performVerification,
                                   false /* expectRecipientStateMachine */),
        DBException,
        ErrorCodes::InvalidOptions);
}

TEST_F(ReshardingDonorRecipientCommonTest, ProcessReshardingFieldsWithoutDonorOrRecipientFields) {
    OperationContext* opCtx = operationContext();
    auto metadata =
        makeShardedMetadataForTemporaryReshardingCollection(opCtx, kThisShard.getShardId());
    ScopedSetShardRole scopedSetShardRole{opCtx,
                                          kTemporaryReshardingNss,
                                          ShardVersionFactory::make(metadata) /* shardVersion */,
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
            const auto csr = CollectionShardingRuntime::acquireShared(opCtx, nss);
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
            const auto csr = CollectionShardingRuntime::acquireShared(opCtx, nss);
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
        const auto csr = CollectionShardingRuntime::acquireShared(opCtx, nss);
        ASSERT(csr->getCurrentMetadataIfKnown() == boost::none);
    }

    doSetupFunc();
    // Add a resharding recipient document that targets the namespaces involved in resharding.
    ReshardingRecipientDocument recipDoc = makeRecipientStateDoc();
    ReshardingRecipientService::RecipientStateMachine::insertStateDocument(opCtx, recipDoc);

    // Clear the filtering metadata (without scheduling a refresh) and assert the metadata is gone.
    resharding::clearFilteringMetadata(opCtx, scheduleAsyncRefresh);

    for (auto const& nss : {kOriginalNss, kTemporaryReshardingNss}) {
        const auto csr = CollectionShardingRuntime::acquireShared(opCtx, nss);
        ASSERT(csr->getCurrentMetadataIfKnown() == boost::none);
    }
}

TEST_F(ReshardingDonorRecipientCommonInternalsTest, ClearReshardingFilteringMetaDataForActiveOp) {
    OperationContext* opCtx = operationContext();
    NamespaceString sourceNss1 = NamespaceString::createNamespaceString_forTest("db", "one");
    NamespaceString tempReshardingNss1 =
        resharding::constructTemporaryReshardingNss(sourceNss1, UUID::gen());
    NamespaceString sourceNss2 = NamespaceString::createNamespaceString_forTest("db", "two");
    NamespaceString tempReshardingNss2 =
        resharding::constructTemporaryReshardingNss(sourceNss2, UUID::gen());
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
        const auto csr = CollectionShardingRuntime::acquireShared(opCtx, nss);
        ASSERT(csr->getCurrentMetadataIfKnown() == boost::none);
    }

    // Assert that the filtering metadata is not cleared for other operation
    for (auto const& nss : {sourceNss2, tempReshardingNss2}) {
        const auto csr = CollectionShardingRuntime::acquireShared(opCtx, nss);
        ASSERT(csr->getCurrentMetadataIfKnown() != boost::none);
    }
}

TEST_F(ReshardingDonorRecipientCommonTest, ProcessRecipientFieldsForCloningNoRefresh) {
    RAIIServerParameterControllerForTest noRefreshFeatureFlagController(
        "featureFlagReshardingCloneNoRefresh", true);

    testProcessRecipientFields(kThisShard.getShardId() /* shardThatChunkExistsOn*/,
                               kThisShard.getShardId() /* primaryShard */,
                               boost::none /* performVerification */,
                               true /* expectRecipientStateMachine */,
                               RecipientFieldsValidator{});
}

}  // namespace
}  // namespace mongo
