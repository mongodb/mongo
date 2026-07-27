// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_test_fixture.h"
#include "mongo/db/s/resharding/resharding_cumulative_metrics.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace mongo {
namespace {

using resharding_coordinator_test::ExternalStateForTest;
using resharding_coordinator_test::ReshardingCoordinatorServiceTestCommon;

/**
 * Test fixture parameterized by ReshardingProvenanceEnum.
 *
 * Three shards are configured: shard0, shard1, shard2. Per-provenance shard layout:
 *   - kReshardCollection / kRewriteCollection: donors {shard0,shard1}, recipients {shard0,shard1}.
 *   - kMoveCollection: donor {shard0}, recipient {shard2}
 *   - kUnshardCollection: donors {shard0,shard1}, recipient {shard2}.
 */

class ReshardingCoordinatorServiceProvenanceTest
    : public ReshardingCoordinatorServiceTestCommon,
      public ::testing::WithParamInterface<ReshardingProvenanceEnum> {
public:
    std::vector<ShardId> getShardIds() const override {
        return {_shard0, _shard1, _shard2};
    }

    ExternalStateForTest::Options getExternalStateOptions() const override {
        ExternalStateForTest::Options opts;
        for (const auto& id : donorShardIds()) {
            opts.documentsToCopy.emplace(id, 50);
            opts.documentsDelta.emplace(id, 5);
        }
        return opts;
    }

    std::vector<ShardId> donorShardIds() const {
        return resharding::isMoveCollection(GetParam()) ? std::vector<ShardId>{_shard0}
                                                        : std::vector<ShardId>{_shard0, _shard1};
    }

    std::vector<ShardId> recipientShardIds() const {
        if (resharding::isMoveCollection(GetParam()) ||
            resharding::isUnshardCollection(GetParam())) {
            return {_shard2};
        }
        return {_shard0, _shard1};
    }

    // True if the coordinator calls _stopMigrations on the source for this provenance.
    bool sourceMigrationsAreBlocked() const {
        return !resharding::isMoveCollection(GetParam());
    }

    ReshardingCumulativeMetrics* metricsBucket() const {
        if (resharding::isOrdinaryReshardCollection(GetParam()))
            return ReshardingCumulativeMetrics::getForResharding(getServiceContext());
        if (resharding::isMoveCollection(GetParam()))
            return ReshardingCumulativeMetrics::getForMoveCollection(getServiceContext());
        if (resharding::isUnshardCollection(GetParam()))
            return ReshardingCumulativeMetrics::getForUnshardCollection(getServiceContext());
        if (resharding::isRewriteCollection(GetParam()))
            return ReshardingCumulativeMetrics::getForRewriteCollection(getServiceContext());
        MONGO_UNREACHABLE;
    }

    ReshardingCoordinatorDocument makeCoordinatorDoc() {
        std::vector<DonorShardEntry> donors;
        for (auto&& id : donorShardIds()) {
            donors.push_back(DonorShardEntry{id, {}});
        }
        std::vector<RecipientShardEntry> recipients;
        for (auto&& id : recipientShardIds()) {
            recipients.push_back(RecipientShardEntry{id, {}});
        }

        CommonReshardingMetadata meta(
            _reshardingUUID, _originalNss, _originalUUID, _tempNss, _newShardKey.toBSON());
        meta.setStartTime(getServiceContext()->getFastClockSource()->now());
        meta.setProvenance(GetParam());

        ForwardableOperationMetadata fom;
        fom.setVersionContext(
            VersionContext{serverGlobalParams.featureCompatibility.acquireFCVSnapshot()});
        meta.setForwardableOpMetadata(std::move(fom));

        ReshardingCoordinatorDocument doc(CoordinatorStateEnum::kUnused, donors, recipients);
        doc.setCommonReshardingMetadata(meta);
        resharding::emplaceCloneTimestampIfExists(doc, _cloneTimestamp);
        doc.setDemoMode(true);

        // moveCollection / unshardCollection consult shardDistribution to determine the
        // destination shard during _isReshardingOpRedundant.
        if (resharding::isMoveCollection(GetParam()) ||
            resharding::isUnshardCollection(GetParam())) {
            ShardKeyRange dest{recipientShardIds().front()};
            dest.setMin(_newShardKey.getKeyPattern().globalMin());
            dest.setMax(_newShardKey.getKeyPattern().globalMax());
            doc.setShardDistribution(std::vector<ShardKeyRange>{dest});
        }

        return doc;
    }

    void insertCatalogEntries(const ReshardingCoordinatorDocument& doc) {
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);

        CollectionType origColl(_originalNss,
                                _originalEpoch,
                                _originalTimestamp,
                                opCtx->getServiceContext()->getPreciseClockSource()->now(),
                                _originalUUID,
                                _oldShardKey.getKeyPattern());
        client.insert(NamespaceString::kConfigsvrCollectionsNamespace, origColl.toBSON());

        DatabaseType dbDoc(doc.getSourceNss().dbName(),
                           doc.getDonorShards().front().getId(),
                           DatabaseVersion{UUID::gen(), Timestamp(1, 1)});
        client.insert(NamespaceString::kConfigDatabasesNamespace, dbDoc.toBSON());
    }

    // Seed config.chunks for the source collection and return preset reshardedChunks for the
    // recipient layout.
    std::vector<ReshardedChunk> seedSourceChunksAndComputeReshardedChunks() {
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);

        const auto donors = donorShardIds();
        ChunkVersion sourceVersion({_originalEpoch, _originalTimestamp}, {1, 0});
        if (donors.size() == 1) {
            ChunkType chunk(_originalUUID,
                            ChunkRange{_oldShardKey.getKeyPattern().globalMin(),
                                       _oldShardKey.getKeyPattern().globalMax()},
                            sourceVersion,
                            donors.front());
            chunk.setName(OID::gen());
            client.insert(NamespaceString::kConfigsvrChunksNamespace, chunk.toConfigBSON());
        } else {
            ChunkType chunk1(
                _originalUUID,
                ChunkRange{_oldShardKey.getKeyPattern().globalMin(), BSON("oldShardKey" << 0)},
                sourceVersion,
                donors[0]);
            chunk1.setName(OID::gen());
            sourceVersion.incMinor();
            ChunkType chunk2(
                _originalUUID,
                ChunkRange{BSON("oldShardKey" << 0), _oldShardKey.getKeyPattern().globalMax()},
                sourceVersion,
                donors[1]);
            chunk2.setName(OID::gen());
            client.insert(NamespaceString::kConfigsvrChunksNamespace, chunk1.toConfigBSON());
            client.insert(NamespaceString::kConfigsvrChunksNamespace, chunk2.toConfigBSON());
        }

        // Build the preset reshardedChunks across recipientShardIds().
        std::vector<ReshardedChunk> presetChunks;
        const auto recipients = recipientShardIds();
        if (recipients.size() == 1) {
            presetChunks.emplace_back(recipients.front(),
                                      _newShardKey.getKeyPattern().globalMin(),
                                      _newShardKey.getKeyPattern().globalMax());
        } else {
            presetChunks.emplace_back(
                recipients[0], _newShardKey.getKeyPattern().globalMin(), BSON("newShardKey" << 0));
            presetChunks.emplace_back(
                recipients[1], BSON("newShardKey" << 0), _newShardKey.getKeyPattern().globalMax());
        }
        return presetChunks;
    }

protected:
    const ShardId _shard0{"shard0000"};
    const ShardId _shard1{"shard0001"};
    const ShardId _shard2{"shard0002"};
};

INSTANTIATE_TEST_SUITE_P(Provenance,
                         ReshardingCoordinatorServiceProvenanceTest,
                         ::testing::Values(ReshardingProvenanceEnum::kReshardCollection,
                                           ReshardingProvenanceEnum::kRewriteCollection,
                                           ReshardingProvenanceEnum::kMoveCollection,
                                           ReshardingProvenanceEnum::kUnshardCollection),
                         [](const ::testing::TestParamInfo<ReshardingProvenanceEnum>& info) {
                             return std::string(idl::serialize(info.param));
                         });

// Drives the full coordinator state machine to kDone for each provenance and asserts the
// per-provenance side effects.
TEST_P(ReshardingCoordinatorServiceProvenanceTest, FullLifecycleSucceeds) {
    auto opCtx = operationContext();

    auto doc = makeCoordinatorDoc();
    insertCatalogEntries(doc);
    auto presetChunks = seedSourceChunksAndComputeReshardedChunks();
    doc.setPresetReshardedChunks(presetChunks);

    auto coordinator =
        ReshardingCoordinator::getOrCreate(opCtx, _service, doc.toBSON(), FixedFCVRegion{opCtx});

    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kPreparingToDonate);
    if (sourceMigrationsAreBlocked()) {
        ASSERT_FALSE(getCollectionCatalogEntry(opCtx).getAllowChunkOperations());
    } else {
        ASSERT_TRUE(getCollectionCatalogEntry(opCtx).getAllowChunkOperations());
    }
    makeDonorsReadyToDonateWithAssert(opCtx);

    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kCloning);
    makeRecipientsFinishedCloningWithAssert(opCtx);

    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kApplying);
    coordinator->onOkayToEnterCritical();

    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kBlockingWrites);
    makeRecipientsBeInStrictConsistencyWithAssert(opCtx);

    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kCommitting);
    makeDonorsProceedToDoneWithAssert(opCtx);
    makeRecipientsProceedToDoneWithAssert(opCtx);

    coordinator->getCompletionFuture().get(opCtx);

    // After commit the source collection's reshardingFields are removed and chunk operations are
    // re-allowed (default true).
    ASSERT_TRUE(getCollectionCatalogEntry(opCtx).getAllowChunkOperations());

    if (resharding::isUnshardCollection(GetParam())) {
        ASSERT_TRUE(getCollectionCatalogEntry(opCtx).getUnsplittable());
    }

    BSONObjBuilder bob;
    metricsBucket()->reportForServerStatus(&bob);
    auto metricsReport = bob.obj();
    auto bucketMetrics = metricsReport.firstElement().Obj();
    ASSERT_EQ(bucketMetrics["countStarted"].numberInt(), 1);
    ASSERT_EQ(bucketMetrics["countSucceeded"].numberInt(), 1);
}

}  // namespace
}  // namespace mongo
