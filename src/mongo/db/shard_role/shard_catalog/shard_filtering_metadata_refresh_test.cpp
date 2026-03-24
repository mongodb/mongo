/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const std::string kShardKey = "_id";
const BSONObj kShardKeyPattern = BSON(kShardKey << 1);

std::pair<CollectionType, std::vector<ChunkType>> makeShardedMetadataForDisk(
    OperationContext* opCtx, int nChunks, ShardId shardId) {
    const UUID uuid = UUID::gen();
    const OID epoch = OID::gen();
    const Timestamp timestamp(Date_t::now());

    CollectionType collType{kTestNss, epoch, timestamp, Date_t::now(), uuid, kShardKeyPattern};

    std::vector<ChunkType> chunks;
    auto chunkVersion = ChunkVersion({epoch, timestamp}, {1, 0});
    for (int i = 0; i < nChunks; i++) {
        auto min = i == 0 ? BSON(kShardKey << MINKEY) : BSON(kShardKey << (i * 100));
        auto max =
            i == (nChunks - 1) ? BSON(kShardKey << MAXKEY) : BSON(kShardKey << ((i + 1) * 100));
        auto range = ChunkRange(min, max);
        auto& chunkInserted = chunks.emplace_back(uuid, std::move(range), chunkVersion, shardId);
        chunkInserted.setName(OID::gen());
        chunkVersion.incMajor();
    }

    return {std::move(collType), std::move(chunks)};
}

CollectionMetadata makeShardedMetadataInMemory(OperationContext* opCtx,
                                               UUID uuid = UUID::gen(),
                                               ShardId chunkShardId = ShardId("other"),
                                               ShardId collectionShardId = ShardId("0")) {
    const OID epoch = OID::gen();
    const Timestamp timestamp(Date_t::now());

    auto range = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY));
    auto chunk =
        ChunkType(uuid, std::move(range), ChunkVersion({epoch, timestamp}, {1, 0}), chunkShardId);
    CurrentChunkManager cm(ShardServerTestFixture::makeStandaloneRoutingTableHistory(
        RoutingTableHistory::makeNew(kTestNss,
                                     uuid,
                                     kShardKeyPattern,
                                     false,
                                     nullptr,
                                     false,
                                     epoch,
                                     timestamp,
                                     boost::none,
                                     boost::none,
                                     true,
                                     {std::move(chunk)})));

    return CollectionMetadata(std::move(cm), collectionShardId);
}

class AuthoritativeRefreshFixture : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();

        auto* vc = VectorClock::get(getServiceContext());
        vc->advanceConfigTime_forTest(LogicalTime(Timestamp(100, 1)));
    }

    void populateDiskCatalog(OperationContext* opCtx,
                             const CollectionType& collType,
                             const std::vector<ChunkType>& chunks) {
        createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
        createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

        {
            DBDirectClient client(opCtx);
            client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace,
                          collType.toBSON());
        }

        for (const auto& chunk : chunks) {
            DBDirectClient client(opCtx);
            client.insert(NamespaceString::kConfigShardCatalogChunksNamespace,
                          chunk.toConfigBSON());
        }
    }

    void populateDiskCatalogIfNeeded(OperationContext* opCtx,
                                     const CollectionType& collType,
                                     const std::vector<ChunkType>& chunks) {
        auto status = createTestCollectionNoThrow(
            opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
        if (!status.isOK() && status.code() != ErrorCodes::NamespaceExists)
            uassertStatusOK(status);
        status =
            createTestCollectionNoThrow(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);
        if (!status.isOK() && status.code() != ErrorCodes::NamespaceExists)
            uassertStatusOK(status);

        {
            DBDirectClient client(opCtx);
            client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace,
                          collType.toBSON());
        }

        for (const auto& chunk : chunks) {
            DBDirectClient client(opCtx);
            client.insert(NamespaceString::kConfigShardCatalogChunksNamespace,
                          chunk.toConfigBSON());
        }
    }

    Status callOnVersionMismatch(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 boost::optional<ChunkVersion> chunkVersionReceived) {
        return FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
            opCtx, nss, chunkVersionReceived);
    }

    BSONObj getStatistics(OperationContext* opCtx) {
        auto& shardingStatistics = ShardingStatistics::get(operationContext());
        BSONObjBuilder builder;
        shardingStatistics.report(&builder);
        auto fullMetrics = builder.obj();
        return fullMetrics.getObjectField("collectionShardingMetadataRecoveryStatistics")
            .getOwned();
    };
};

// ---------------------------------------------------------------------------
// Basic path coverage
// ---------------------------------------------------------------------------

TEST_F(AuthoritativeRefreshFixture, UntrackedIsCorrectlyRecoveredFromDisk) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto untrackedNss = NamespaceString::createNamespaceString_forTest("local", "oplog.rs");

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, untrackedNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
    }

    auto status = callOnVersionMismatch(opCtx, untrackedNss, boost::none);
    ASSERT_OK(status);
    auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, untrackedNss);
    ASSERT_TRUE(csr->getCurrentMetadataIfKnown());
}

TEST_F(AuthoritativeRefreshFixture, NoChunkVersionTriggersRecoveryFromDisk) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
    }

    auto status = callOnVersionMismatch(opCtx, kTestNss, boost::none);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
}

TEST_F(AuthoritativeRefreshFixture, ChunkVersionMatchReturnsEarly) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    auto metadata = makeShardedMetadataInMemory(opCtx);
    auto matchingVersion = metadata.getCollPlacementVersion();

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->setFilteringMetadata_authoritative(opCtx, metadata);
    }

    auto status = callOnVersionMismatch(opCtx, kTestNss, matchingVersion);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    ASSERT_TRUE(csr->getCurrentMetadataIfKnown().has_value());
    ASSERT_EQ(csr->getCurrentMetadataIfKnown()->getCollPlacementVersion(), matchingVersion);
    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 0);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 0);
}

TEST_F(AuthoritativeRefreshFixture, ConfigTimeReachedWithMetadataKnownReturnsWithoutRecovery) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    auto metadata = makeShardedMetadataInMemory(opCtx);
    auto currentVersion = metadata.getCollPlacementVersion();

    auto higherVersion = currentVersion;
    higherVersion.incMajor();

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->setFilteringMetadata_authoritative(opCtx, metadata);
    }

    auto status = callOnVersionMismatch(opCtx, kTestNss, higherVersion);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    ASSERT_TRUE(csr->getCurrentMetadataIfKnown().has_value());
    ASSERT_EQ(csr->getCurrentMetadataIfKnown()->getCollPlacementVersion(), currentVersion);
    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 0);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 0);
}

TEST_F(AuthoritativeRefreshFixture, ConfigTimeReachedWithEmptyCSRTriggersFullRecovery) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    auto dummyVersion = ChunkVersion({OID::gen(), Timestamp(50, 1)}, {1, 0});

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
    }

    auto status = callOnVersionMismatch(opCtx, kTestNss, dummyVersion);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 1);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 1);
}

// ---------------------------------------------------------------------------
// Non-authoritative transition
// ---------------------------------------------------------------------------

TEST_F(AuthoritativeRefreshFixture, NonAuthoritativeTransitionDuringRecoveryReturnsEarly) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    auto dummyVersion = ChunkVersion({OID::gen(), Timestamp(50, 1)}, {1, 0});

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
        csr->enterCriticalSectionCatchUpPhase(opCtx, BSONObj());
        csr->enterCriticalSectionCommitPhase(opCtx, BSONObj());
    }

    stdx::thread recoveryThread([&] {
        auto bgClient = getGlobalServiceContext()->getService()->makeClient("bgFlip");
        auto bgOpCtx = bgClient->makeOperationContext();
        auto bgStatus = callOnVersionMismatch(bgOpCtx.get(), kTestNss, dummyVersion);
        ASSERT_OK(bgStatus);
    });

    stdx::this_thread::sleep_for(stdx::chrono::milliseconds(200));

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->setFilteringMetadata_nonAuthoritative(opCtx, CollectionMetadata::UNTRACKED());
        csr->exitCriticalSection(opCtx, BSONObj());
    }

    recoveryThread.join();

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    ASSERT_EQ(csr->getAuthoritativeState(),
              CollectionShardingRuntime::AuthoritativeState::kNonAuthoritative);
}

// ---------------------------------------------------------------------------
// Critical section interactions
// ---------------------------------------------------------------------------

TEST_F(AuthoritativeRefreshFixture, CriticalSectionBlocksRecoveryThenProceeds) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    auto dummyVersion = ChunkVersion({OID::gen(), Timestamp(50, 1)}, {1, 0});

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
        csr->enterCriticalSectionCatchUpPhase(opCtx, BSONObj());
        csr->enterCriticalSectionCommitPhase(opCtx, BSONObj());
    }

    stdx::thread recoveryThread([&] {
        auto bgClient = getGlobalServiceContext()->getService()->makeClient("bgClient");
        auto bgOpCtx = bgClient->makeOperationContext();
        auto bgStatus = callOnVersionMismatch(bgOpCtx.get(), kTestNss, dummyVersion);
        ASSERT_OK(bgStatus);
    });

    stdx::this_thread::sleep_for(stdx::chrono::milliseconds(200));

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->exitCriticalSection(opCtx, BSONObj());
    }

    recoveryThread.join();

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
}

// ---------------------------------------------------------------------------
// Recoverer lifecycle and joining
// ---------------------------------------------------------------------------

TEST_F(AuthoritativeRefreshFixture, ExistingRecovererIsJoinedRatherThanCreatingNew) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    auto dummyVersion = ChunkVersion({OID::gen(), Timestamp(50, 1)}, {1, 0});

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
        auto recoverer = std::make_shared<CollectionCacheRecoverer>(kTestNss);
        csr->setCollectionRecoverer(std::move(recoverer));
    }

    auto status = callOnVersionMismatch(opCtx, kTestNss, dummyVersion);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 0);
}

TEST_F(AuthoritativeRefreshFixture, RecovererCleanedUpAfterRecovery) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 3, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
    }

    auto status = callOnVersionMismatch(opCtx, kTestNss, boost::none);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    ASSERT_TRUE(csr->getCurrentMetadataIfKnown().has_value());
    ASSERT_FALSE(csr->getCollectionCacheRecoverer());
}

// ---------------------------------------------------------------------------
// Concurrency stress tests
// ---------------------------------------------------------------------------

TEST_F(AuthoritativeRefreshFixture, ThreeConcurrentCallersAllSucceed) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 10, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    auto dummyVersion = ChunkVersion({OID::gen(), Timestamp(50, 1)}, {1, 0});

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
    }

    Notification<void> ready1, ready2, ready3;
    auto makeThread = [&](Notification<void>& ready, const char* name) {
        return stdx::thread([&, name] {
            auto client = getGlobalServiceContext()->getService()->makeClient(name);
            auto opCtx = client->makeOperationContext();
            ready.set();
            auto status = callOnVersionMismatch(opCtx.get(), kTestNss, dummyVersion);
            ASSERT_OK(status);
        });
    };

    auto t1 = makeThread(ready1, "t1");
    auto t2 = makeThread(ready2, "t2");
    auto t3 = makeThread(ready3, "t3");

    ready1.get();
    ready2.get();
    ready3.get();

    t1.join();
    t2.join();
    t3.join();

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
    ASSERT_FALSE(csr->getCollectionCacheRecoverer());
}

// ---------------------------------------------------------------------------
// Metadata shape and content verification
// ---------------------------------------------------------------------------

TEST_F(AuthoritativeRefreshFixture, RecoveryWithSingleChunkVerifiesExactMetadata) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 1, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
    }

    auto status = callOnVersionMismatch(opCtx, kTestNss, boost::none);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_TRUE(metadataOpt->isSharded());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
    ASSERT_BSONOBJ_EQ(metadataOpt->getKeyPattern(), kShardKeyPattern);
}

TEST_F(AuthoritativeRefreshFixture, RecoveryWithManyChunksVerifiesVersionSorting) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 100, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
    }

    auto status = callOnVersionMismatch(opCtx, kTestNss, boost::none);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
    ASSERT_EQ(metadataOpt->getShardPlacementVersion(), chunks.back().getVersion());
}

TEST_F(AuthoritativeRefreshFixture,
       RecoveryWithChunksOnDifferentShardReportsUntrackedForThisShard) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    // All chunks belong to "otherShard", not kMyShardName.
    const ShardId otherShard("otherShard");
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 3, otherShard);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
    }

    auto status = callOnVersionMismatch(opCtx, kTestNss, boost::none);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_TRUE(metadataOpt->isSharded());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
    ASSERT_FALSE(metadataOpt->getShardPlacementVersion().isSet());
}

// ---------------------------------------------------------------------------
// Idempotency and re-recovery
// ---------------------------------------------------------------------------

TEST_F(AuthoritativeRefreshFixture, SequentialCallsAreIdempotent) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    auto dummyVersion = ChunkVersion({OID::gen(), Timestamp(50, 1)}, {1, 0});

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
    }

    // First call: recovers from disk.
    ASSERT_OK(callOnVersionMismatch(opCtx, kTestNss, dummyVersion));

    auto recoveredVersion = [&] {
        auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
        return csr->getCurrentMetadataIfKnown()->getCollPlacementVersion();
    }();
    ASSERT_EQ(recoveredVersion, chunks.back().getVersion());

    // Second call with the same (stale) version: configTime resolves first, but the recovery
    // loop sees metadata is already known (case 2 at line 1113) and returns immediately.
    ASSERT_OK(callOnVersionMismatch(opCtx, kTestNss, dummyVersion));

    auto secondVersion = [&] {
        auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
        return csr->getCurrentMetadataIfKnown()->getCollPlacementVersion();
    }();
    ASSERT_EQ(secondVersion, recoveredVersion);

    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 1);
}

TEST_F(AuthoritativeRefreshFixture, RecoveredVersionMatchSkipsRecoveryLoopOnNextCall) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
    }

    // First call: recover from disk using boost::none (no version match possible).
    ASSERT_OK(callOnVersionMismatch(opCtx, kTestNss, boost::none));

    auto recoveredVersion = [&] {
        auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
        return csr->getCurrentMetadataIfKnown()->getCollPlacementVersion();
    }();

    // Second call: pass the recovered version. The chunk version waiter resolves immediately and
    // we never enter the recovery loop.
    ASSERT_OK(callOnVersionMismatch(opCtx, kTestNss, recoveredVersion));

    auto finalVersion = [&] {
        auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
        return csr->getCurrentMetadataIfKnown()->getCollPlacementVersion();
    }();
    ASSERT_EQ(finalVersion, recoveredVersion);

    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 1);
}

TEST_F(AuthoritativeRefreshFixture, ReRecoveryAfterMetadataCleared) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
    }

    // First recovery.
    ASSERT_OK(callOnVersionMismatch(opCtx, kTestNss, boost::none));

    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 1);
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 1);

    {
        auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
        ASSERT_TRUE(csr->getCurrentMetadataIfKnown().has_value());
    }

    // Simulate a DDL or state change that clears the CSR metadata and the corresponding durable
    // state.
    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
        DBDirectClient client(opCtx);
        client.remove(NamespaceString::kConfigShardCatalogCollectionsNamespace, BSONObj{});
        client.remove(NamespaceString::kConfigShardCatalogChunksNamespace, BSONObj{});
    }
    const auto [collType2, chunks2] = makeShardedMetadataForDisk(opCtx, 2, kMyShardName);
    populateDiskCatalogIfNeeded(opCtx, collType2, chunks2);

    // Second recovery should pick up the new disk state.
    ASSERT_OK(callOnVersionMismatch(opCtx, kTestNss, boost::none));

    stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 2);
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 2);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks2.back().getVersion());
    ASSERT_NE(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
}

// ---------------------------------------------------------------------------
// Critical section + metadata installation interaction
// ---------------------------------------------------------------------------

TEST_F(AuthoritativeRefreshFixture,
       CriticalSectionExitedWithMetadataInstalledExternallyReturnsEarly) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    auto dummyVersion = ChunkVersion({OID::gen(), Timestamp(50, 1)}, {1, 0});

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
        csr->enterCriticalSectionCatchUpPhase(opCtx, BSONObj());
        csr->enterCriticalSectionCommitPhase(opCtx, BSONObj());
    }

    stdx::thread recoveryThread([&] {
        auto bgClient = getGlobalServiceContext()->getService()->makeClient("bgCS");
        auto bgOpCtx = bgClient->makeOperationContext();
        auto bgStatus = callOnVersionMismatch(bgOpCtx.get(), kTestNss, dummyVersion);
        ASSERT_OK(bgStatus);
    });

    stdx::this_thread::sleep_for(stdx::chrono::milliseconds(200));

    // While the recovery thread is blocked on the critical section, install metadata externally and
    // exit the critical section. The bg thread will re-enter the loop, find metadata already known,
    // and return without triggering disk recovery.
    auto installedMetadata = makeShardedMetadataInMemory(opCtx);
    auto installedVersion = installedMetadata.getCollPlacementVersion();
    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->setFilteringMetadata_authoritative(opCtx, installedMetadata);
        csr->exitCriticalSection(opCtx, BSONObj());
    }

    recoveryThread.join();

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), installedVersion);
    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 0);
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 0);
}

}  // namespace
}  // namespace mongo
