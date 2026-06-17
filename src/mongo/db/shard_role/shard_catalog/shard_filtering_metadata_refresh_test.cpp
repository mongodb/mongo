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

#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/commit_collection_metadata_locally.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state_mock.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/db/version_context.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/log_capture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

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

    // These tests mutate authoritative state and then wait on majority-read-based recovery. Advance
    // the committed snapshot so the next durable read does not keep observing an older snapshot.
    void advanceCommittedSnapshot(OperationContext* opCtx) {
        auto opTime = replicationCoordinator()->getMyLastWrittenOpTime();
        if (opTime.isNull()) {
            opTime = repl::OpTime(Timestamp(1, 1), 0);
            replicationCoordinator()->setMyLastWrittenOpTimeAndWallTimeForward({opTime, Date_t()});
        }
        replicationCoordinator()->setCurrentCommittedSnapshotOpTime(opTime);
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

    Status onShardVersionMismatch(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  boost::optional<ChunkVersion> receivedShardVersion) {
        return FilteringMetadataCache::get(opCtx)->onShardVersionMismatch(
            opCtx, nss, receivedShardVersion);
    }

    BSONObj getStatistics(OperationContext* opCtx) {
        auto& shardingStatistics = ShardingStatistics::get(operationContext());
        BSONObjBuilder builder;
        shardingStatistics.report(&builder);
        auto fullMetrics = builder.obj();
        return fullMetrics.getObjectField("collectionShardingMetadataRecoveryStatistics")
            .getOwned();
    };

    unittest::ServerParameterGuard crudFeatureFlag{"featureFlagAuthoritativeShardsCRUD", true};
    unittest::ServerParameterGuard ddlFeatureFlag{"featureFlagAuthoritativeShardsDDL", true};
};

TEST_F(AuthoritativeRefreshFixture, UntrackedIsCorrectlyRecoveredFromDisk) {
    auto* opCtx = operationContext();

    const auto untrackedNss =
        NamespaceString::createNamespaceString_forTest("TestDB", "UntrackedColl");

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, untrackedNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    auto status = onShardVersionMismatch(opCtx, untrackedNss, boost::none);
    ASSERT_OK(status);
    auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, untrackedNss);
    ASSERT_TRUE(csr->getCurrentMetadataIfKnown());
    ASSERT_FALSE(csr->getCurrentMetadataIfKnown()->isSharded());
}

TEST_F(AuthoritativeRefreshFixture, NoChunkVersionTriggersRecoveryFromDisk) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    auto status = onShardVersionMismatch(opCtx, kTestNss, boost::none);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
}

TEST_F(AuthoritativeRefreshFixture, ChunkVersionMatchReturnsEarly) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    auto matchingVersion = chunks.back().getVersion();

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    auto status = onShardVersionMismatch(opCtx, kTestNss, matchingVersion);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    ASSERT_TRUE(csr->getCurrentMetadataIfKnown().has_value());
    ASSERT_EQ(csr->getCurrentMetadataIfKnown()->getCollPlacementVersion(), matchingVersion);
    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 1);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 1);
    ASSERT_EQ(stats.getIntField("versionResolvedAfterRecovery"), 1);
}

// Refresh uses the node FCV, not the operation's OFCV (see SERVER-128194 for details).
TEST_F(AuthoritativeRefreshFixture, RefreshUsesGlobalFCVRatherThanOperationFCV) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    // Set OFCV=kUpgrading, so refreshes should still be non-authoritative according to OFCV.
    // (Generic FCV reference): used for testing.
    const auto savedFCV = serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ON_BLOCK_EXIT([&] { serverGlobalParams.mutableFCV.setVersion(savedFCV); });
    serverGlobalParams.mutableFCV.setVersion(
        multiversion::GenericFCV::kUpgradingFromLastLTSToLatest);
    VersionContext::FixedOperationFCVRegion fixedOperationFcvRegion(opCtx);
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);

    // Despite the OFCV, the refresh recovers authoritatively (from disk) per the node FCV.
    ASSERT_OK(onShardVersionMismatch(opCtx, kTestNss, boost::none));
    ASSERT_EQ(getStatistics(opCtx).getIntField("diskRecoveriesPerformed"), 1);
}

TEST_F(AuthoritativeRefreshFixture,
       UntrackedRouterVersionWithKnownTrackedMetadataSkipsSecondDiskRecovery) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    ASSERT_OK(onShardVersionMismatch(opCtx, kTestNss, boost::none));

    auto statsAfterFirst = getStatistics(opCtx);
    ASSERT_EQ(statsAfterFirst.getIntField("diskRecoveriesPerformed"), 1);
    ASSERT_EQ(statsAfterFirst.getIntField("recoverersCreated"), 1);

    ASSERT_OK(onShardVersionMismatch(opCtx, kTestNss, ChunkVersion::UNTRACKED()));

    auto statsAfterSecond = getStatistics(opCtx);
    ASSERT_EQ(statsAfterSecond.getIntField("diskRecoveriesPerformed"), 1)
        << "Second placement mismatch should not re-run disk recovery when CSS already has "
           "metadata";
    ASSERT_EQ(statsAfterSecond.getIntField("recoverersCreated"), 1);
}

TEST_F(AuthoritativeRefreshFixture,
       IgnoredReceivedVersionResolvesAfterRecoveryWithoutPostRecoveryWait) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    auto status = onShardVersionMismatch(opCtx, kTestNss, ChunkVersion::IGNORED());
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    ASSERT_TRUE(csr->getCurrentMetadataIfKnown().has_value());
    ASSERT_EQ(csr->getCurrentMetadataIfKnown()->getCollPlacementVersion(),
              chunks.back().getVersion());
    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 1);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 1);
    ASSERT_EQ(stats.getIntField("versionResolvedAfterRecovery"), 1);
    ASSERT_EQ(stats.getIntField("postRecoveryWaitResolvedByConfigTime"), 0);
    ASSERT_EQ(stats.getIntField("postRecoveryWaitResolvedByVersionChange"), 0);
}

TEST_F(AuthoritativeRefreshFixture, HigherRouterVersionTriggersRecoveryThenConfigTimeWait) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    auto currentVersion = chunks.back().getVersion();
    auto higherVersion = currentVersion;
    higherVersion.incMajor();

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    auto status = onShardVersionMismatch(opCtx, kTestNss, higherVersion);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    ASSERT_TRUE(csr->getCurrentMetadataIfKnown().has_value());
    ASSERT_EQ(csr->getCurrentMetadataIfKnown()->getCollPlacementVersion(), currentVersion);
    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 1);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 1);
    ASSERT_EQ(stats.getIntField("postRecoveryWaitResolvedByConfigTime"), 1);
}

TEST_F(AuthoritativeRefreshFixture, ConfigTimeReachedWithEmptyCSRTriggersFullRecovery) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    auto dummyVersion = ChunkVersion({OID::gen(), Timestamp(50, 1)}, {1, 0});

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    auto status = onShardVersionMismatch(opCtx, kTestNss, dummyVersion);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 1);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 1);
}

TEST_F(AuthoritativeRefreshFixture, NonAuthoritativeTransitionDuringRecoveryReturnsEarly) {
    auto* opCtx = operationContext();

    auto dummyVersion = ChunkVersion({OID::gen(), Timestamp(50, 1)}, {1, 0});

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
        csr->enterCriticalSectionCatchUpPhase(opCtx, BSONObj());
        csr->enterCriticalSectionCommitPhase(opCtx, BSONObj());
    }

    auto* fp = globalFailPointRegistry().find("hangBeforePlacementVersionCriticalSectionWait");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);

    stdx::thread recoveryThread([&] {
        auto bgClient = getGlobalServiceContext()->getService()->makeClient("bgFlip");
        auto bgOpCtx = bgClient->makeOperationContext();
        auto bgStatus = onShardVersionMismatch(bgOpCtx.get(), kTestNss, dummyVersion);
        ASSERT_OK(bgStatus);
    });

    fp->waitForTimesEntered(initialTimesEntered + 1);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->setCollectionMetadata(opCtx, CollectionMetadata::UNTRACKED());
        csr->setNonAuthoritative();
        csr->exitCriticalSection(opCtx, BSONObj());
    }

    advanceCommittedSnapshot(opCtx);

    fp->setMode(FailPoint::off);

    recoveryThread.join();

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    ASSERT_EQ(csr->getAuthoritativeState(),
              CollectionShardingRuntime::AuthoritativeState::kNonAuthoritative);
}

TEST_F(AuthoritativeRefreshFixture, CriticalSectionBlocksRecoveryThenProceeds) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    auto dummyVersion = ChunkVersion({OID::gen(), Timestamp(50, 1)}, {1, 0});

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
        csr->enterCriticalSectionCatchUpPhase(opCtx, BSONObj());
        csr->enterCriticalSectionCommitPhase(opCtx, BSONObj());
    }

    auto* fp = globalFailPointRegistry().find("hangBeforePlacementVersionCriticalSectionWait");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);

    stdx::thread recoveryThread([&] {
        auto bgClient = getGlobalServiceContext()->getService()->makeClient("bgClient");
        auto bgOpCtx = bgClient->makeOperationContext();
        auto bgStatus = onShardVersionMismatch(bgOpCtx.get(), kTestNss, dummyVersion);
        ASSERT_OK(bgStatus);
    });

    fp->waitForTimesEntered(initialTimesEntered + 1);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->exitCriticalSection(opCtx, BSONObj());
    }

    fp->setMode(FailPoint::off);

    recoveryThread.join();

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
}

TEST_F(AuthoritativeRefreshFixture, CollectionCriticalSectionWaitDoesNotCountAsNoProgress) {
    unittest::ServerParameterGuard maxAttempts("maxShardMetadataDiskRecoveryAttempts", 1);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    auto dummyVersion = ChunkVersion({OID::gen(), Timestamp(50, 1)}, {1, 0});

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
        csr->enterCriticalSectionCatchUpPhase(opCtx, BSONObj());
        csr->enterCriticalSectionCommitPhase(opCtx, BSONObj());
    }

    auto* fp = globalFailPointRegistry().find("hangBeforePlacementVersionCriticalSectionWait");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);

    stdx::thread recoveryThread([&] {
        auto bgClient = getGlobalServiceContext()->getService()->makeClient("bgCriticalSection");
        auto bgOpCtx = bgClient->makeOperationContext();
        ASSERT_OK(onShardVersionMismatch(bgOpCtx.get(), kTestNss, dummyVersion));
    });

    fp->waitForTimesEntered(initialTimesEntered + 1);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->exitCriticalSection(opCtx, BSONObj());
    }

    fp->setMode(FailPoint::off);
    recoveryThread.join();

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
}

using AuthoritativeRefreshFixtureDeathTest = AuthoritativeRefreshFixture;
DEATH_TEST_REGEX_F(AuthoritativeRefreshFixtureDeathTest,
                   PostDrainRetryCountsAgainstNoProgressBudget,
                   "Tripwire assertion.*Exhausted maximum number") {
    unittest::ServerParameterGuard maxAttempts("maxShardMetadataDiskRecoveryAttempts", 1);
    auto* opCtx = operationContext();

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    uassertStatusOK(onShardVersionMismatch(opCtx, kTestNss, boost::none));
}

TEST_F(AuthoritativeRefreshFixture, ClearFilteringMetadataDuringPostRecoveryWaitTriggersRetry) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    // Force the recovery thread to ignore the majority waiter since it's an immediately fulfilled
    // future in unit tests.
    auto* fp = globalFailPointRegistry().find("forceWaitForVersionOnly");
    auto initialTimesEntered = fp->setMode(FailPoint::nTimes, 1);

    // Router reports UNTRACKED while the shard's on-disk metadata is tracked: the post-recovery
    // comparison cannot order the two, so recovery falls into Step 4's wait.
    stdx::thread recoveryThread([&] {
        auto bgClient = getGlobalServiceContext()->getService()->makeClient("bgInterrupt");
        auto bgOpCtx = bgClient->makeOperationContext();
        ASSERT_OK(onShardVersionMismatch(bgOpCtx.get(), kTestNss, ChunkVersion::UNTRACKED()));
    });

    fp->waitForTimesEntered(initialTimesEntered + 1);

    // Cancel the registered version waiter and clear the CSS metadata. The wait should observe
    // CallbackCanceled and return kYes so the outer recovery loop performs another disk
    // recovery iteration.
    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    fp->setMode(FailPoint::off);

    recoveryThread.join();

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());

    // The retry path must have run disk recovery a second time: once for the original wait that was
    // interrupted, and once for the retried iteration that completed.
    auto stats = getStatistics(opCtx);
    ASSERT_GTE(stats.getIntField("diskRecoveriesPerformed"), 2)
        << "Expected the wait interrupt to trigger a second disk recovery";
}

TEST_F(AuthoritativeRefreshFixture, RecoveryCreatesExactlyOneRecoverer) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    auto dummyVersion = ChunkVersion({OID::gen(), Timestamp(50, 1)}, {1, 0});

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    auto status = onShardVersionMismatch(opCtx, kTestNss, dummyVersion);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 1);
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 1);
}

TEST_F(AuthoritativeRefreshFixture, RecovererCleanedUpAfterRecovery) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 3, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    auto status = onShardVersionMismatch(opCtx, kTestNss, boost::none);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    ASSERT_TRUE(csr->getCurrentMetadataIfKnown().has_value());
    ASSERT_FALSE(csr->getCollectionCacheRecoverer());
}

TEST_F(AuthoritativeRefreshFixture, ThreeConcurrentCallersAllSucceed) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 10, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    auto dummyVersion = ChunkVersion({OID::gen(), Timestamp(50, 1)}, {1, 0});

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    Notification<void> ready1, ready2, ready3;
    auto makeThread = [&](Notification<void>& ready, const char* name) {
        return stdx::thread([&, name] {
            auto client = getGlobalServiceContext()->getService()->makeClient(name);
            auto opCtx = client->makeOperationContext();
            ready.set();
            auto status = onShardVersionMismatch(opCtx.get(), kTestNss, dummyVersion);
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

TEST_F(AuthoritativeRefreshFixture, RecoveryWithSingleChunkVerifiesExactMetadata) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 1, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    auto status = onShardVersionMismatch(opCtx, kTestNss, boost::none);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_TRUE(metadataOpt->isSharded());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
    ASSERT_BSONOBJ_EQ(metadataOpt->getKeyPattern(), kShardKeyPattern);
}

TEST_F(AuthoritativeRefreshFixture, RecoveryWithManyChunksVerifiesVersionSorting) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 100, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    auto status = onShardVersionMismatch(opCtx, kTestNss, boost::none);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
    ASSERT_EQ(metadataOpt->getShardPlacementVersion(), chunks.back().getVersion());
}

TEST_F(AuthoritativeRefreshFixture,
       RecoveryWithChunksOnDifferentShardReportsUntrackedForThisShard) {
    auto* opCtx = operationContext();

    // All chunks belong to "otherShard", not kMyShardName.
    const ShardId otherShard("otherShard");
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 3, otherShard);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    auto status = onShardVersionMismatch(opCtx, kTestNss, boost::none);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_TRUE(metadataOpt->isSharded());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
    ASSERT_FALSE(metadataOpt->getShardPlacementVersion().isSet());
}

TEST_F(AuthoritativeRefreshFixture, PartialRangeDiskCatalogRecoversWithoutChunkMetadata) {
    auto* opCtx = operationContext();

    const UUID uuid = UUID::gen();
    const OID epoch = OID::gen();
    const Timestamp timestamp(Date_t::now());

    CollectionType collType{kTestNss, epoch, timestamp, Date_t::now(), uuid, kShardKeyPattern};

    // Build two contiguous chunks owned by this shard whose extremes are neither MinKey nor
    // MaxKey. The "missing" chunks [MinKey, 100) and [300, MaxKey] logically exist on
    // "otherShard" but are not persisted on this node, exactly as `fetchOwnedChunks` would
    // produce at runtime.
    auto makeChunk = [&](BSONObj min, BSONObj max, ChunkVersion v) {
        ChunkType c{uuid, ChunkRange(std::move(min), std::move(max)), v, kMyShardName};
        c.setName(OID::gen());
        return c;
    };

    ChunkVersion v({epoch, timestamp}, {1, 0});
    std::vector<ChunkType> myOwnedChunks;
    myOwnedChunks.push_back(makeChunk(BSON(kShardKey << 100), BSON(kShardKey << 200), v));
    v.incMajor();
    myOwnedChunks.push_back(makeChunk(BSON(kShardKey << 200), BSON(kShardKey << 300), v));

    populateDiskCatalog(opCtx, collType, myOwnedChunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    auto status = onShardVersionMismatch(opCtx, kTestNss, boost::none);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_TRUE(metadataOpt->isSharded());
    ASSERT_TRUE(metadataOpt->getShardPlacementVersion().isSet());
    ASSERT_EQ(metadataOpt->getShardPlacementVersion(), myOwnedChunks.back().getVersion());
    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 1);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 1);
}

TEST_F(AuthoritativeRefreshFixture, SequentialCallsAreIdempotent) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    auto dummyVersion = ChunkVersion({OID::gen(), Timestamp(50, 1)}, {1, 0});

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    // First call: recovers from disk.
    ASSERT_OK(onShardVersionMismatch(opCtx, kTestNss, dummyVersion));

    auto recoveredVersion = [&] {
        auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
        return csr->getCurrentMetadataIfKnown()->getCollPlacementVersion();
    }();
    ASSERT_EQ(recoveredVersion, chunks.back().getVersion());

    // Second call with the same stale version: the pre-recovery comparison sees that the current
    // metadata is already newer, so the request returns without creating another recoverer.
    ASSERT_OK(onShardVersionMismatch(opCtx, kTestNss, dummyVersion));

    auto secondVersion = [&] {
        auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
        return csr->getCurrentMetadataIfKnown()->getCollPlacementVersion();
    }();
    ASSERT_EQ(secondVersion, recoveredVersion);

    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 1);
    ASSERT_EQ(stats.getIntField("versionResolvedBeforeRecovery"), 1);
}

TEST_F(AuthoritativeRefreshFixture, RecoveredVersionMatchSkipsRecoveryLoopOnNextCall) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    // First call: recover from disk using boost::none (no version match possible).
    ASSERT_OK(onShardVersionMismatch(opCtx, kTestNss, boost::none));

    auto recoveredVersion = [&] {
        auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
        return csr->getCurrentMetadataIfKnown()->getCollPlacementVersion();
    }();

    // Second call: pass the recovered version. The pre-recovery check (Step 1) finds that the
    // metadata already satisfies the received version, so disk recovery is skipped entirely.
    ASSERT_OK(onShardVersionMismatch(opCtx, kTestNss, recoveredVersion));

    auto finalVersion = [&] {
        auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
        return csr->getCurrentMetadataIfKnown()->getCollPlacementVersion();
    }();
    ASSERT_EQ(finalVersion, recoveredVersion);

    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 1);
    ASSERT_EQ(stats.getIntField("versionResolvedBeforeRecovery"), 1);
}

TEST_F(AuthoritativeRefreshFixture, OngoingRecoverySatisfiesVersionSkipsDiskRecovery) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    auto matchingVersion = chunks.back().getVersion();

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    // Pause the background recovery thread so Thread A's disk recovery is in-flight when Thread B
    // enters the version mismatch handler.
    auto* fp = globalFailPointRegistry().find("hangInRecoverRefreshThread");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);

    Notification<void> threadAStarted;
    stdx::thread threadA([&] {
        auto client = getGlobalServiceContext()->getService()->makeClient("threadA");
        auto threadAOpCtx = client->makeOperationContext();
        threadAStarted.set();
        auto status = onShardVersionMismatch(threadAOpCtx.get(), kTestNss, boost::none);
        ASSERT_OK(status);
    });

    threadAStarted.get();
    fp->waitForTimesEntered(initialTimesEntered + 1);

    // Thread A is now paused inside the recovery thread. Start Thread B with a version that will
    // be satisfied once Thread A's recovery completes and installs metadata.
    Notification<void> threadBStarted;
    stdx::thread threadB([&] {
        auto client = getGlobalServiceContext()->getService()->makeClient("threadB");
        auto threadBOpCtx = client->makeOperationContext();
        threadBStarted.set();
        auto status = onShardVersionMismatch(threadBOpCtx.get(), kTestNss, matchingVersion);
        ASSERT_OK(status);
    });

    threadBStarted.get();

    // Unblock Thread A's recovery. Thread B will join the ongoing refresh, then find the metadata
    // already satisfies its receivedShardVersion — skipping its own disk recovery.
    fp->setMode(FailPoint::off);

    threadA.join();
    threadB.join();

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    ASSERT_TRUE(csr->getCurrentMetadataIfKnown().has_value());
    ASSERT_EQ(csr->getCurrentMetadataIfKnown()->getCollPlacementVersion(), matchingVersion);

    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 1);
    ASSERT_EQ(stats.getIntField("versionResolvedBeforeRecovery"), 1);
}

TEST_F(AuthoritativeRefreshFixture, ReRecoveryAfterMetadataCleared) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    // First recovery.
    ASSERT_OK(onShardVersionMismatch(opCtx, kTestNss, boost::none));

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
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
        DBDirectClient client(opCtx);
        client.remove(NamespaceString::kConfigShardCatalogCollectionsNamespace, BSONObj{});
        client.remove(NamespaceString::kConfigShardCatalogChunksNamespace, BSONObj{});
    }
    const auto [collType2, chunks2] = makeShardedMetadataForDisk(opCtx, 2, kMyShardName);
    populateDiskCatalogIfNeeded(opCtx, collType2, chunks2);

    // Second recovery should pick up the new disk state.
    ASSERT_OK(onShardVersionMismatch(opCtx, kTestNss, boost::none));

    stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 2);
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 2);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), chunks2.back().getVersion());
    ASSERT_NE(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
}

TEST_F(AuthoritativeRefreshFixture, CriticalSectionExitedWithExternalMetadataSkipsDiskRecovery) {
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    auto dummyVersion = ChunkVersion({OID::gen(), Timestamp(50, 1)}, {1, 0});

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
        csr->enterCriticalSectionCatchUpPhase(opCtx, BSONObj());
        csr->enterCriticalSectionCommitPhase(opCtx, BSONObj());
    }

    auto* fp = globalFailPointRegistry().find("hangBeforePlacementVersionCriticalSectionWait");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);

    stdx::thread recoveryThread([&] {
        auto bgClient = getGlobalServiceContext()->getService()->makeClient("bgCS");
        auto bgOpCtx = bgClient->makeOperationContext();
        auto bgStatus = onShardVersionMismatch(bgOpCtx.get(), kTestNss, dummyVersion);
        ASSERT_OK(bgStatus);
    });

    fp->waitForTimesEntered(initialTimesEntered + 1);

    // While the recovery thread is blocked on the critical section, install different metadata
    // externally and exit the critical section. Once the critical section is released, the bg
    // thread re-checks the current metadata and sees that it already satisfies the stale version,
    // so it returns before performing disk recovery.
    auto externalMetadata = makeShardedMetadataInMemory(opCtx);
    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->setCollectionMetadata(
            opCtx, externalMetadata, CollectionShardingRuntime::NoRoutingTableAs::kUntracked);
        csr->setAuthoritative();
        csr->exitCriticalSection(opCtx, BSONObj());
    }

    fp->setMode(FailPoint::off);

    recoveryThread.join();

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_EQ(metadataOpt->getCollPlacementVersion(), externalMetadata.getCollPlacementVersion());
    ASSERT_NE(metadataOpt->getCollPlacementVersion(), chunks.back().getVersion());
    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 0);
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 0);
    ASSERT_EQ(stats.getIntField("versionResolvedBeforeRecovery"), 1);
}

TEST_F(AuthoritativeRefreshFixture, UnownedRecoveryAcceptsTrackedWithNoChunksVersion) {
    auto* opCtx = operationContext();

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    const auto trackedZeroChunksVersion =
        ChunkVersion({OID::gen(), Timestamp(50, 1)}, {0 /* major */, 0 /* minor */});

    auto status = onShardVersionMismatch(opCtx, kTestNss, trackedZeroChunksVersion);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    ASSERT_TRUE(csr->isUnowned())
        << "Expected CSS state kUnowned after recovery of a collection with no on-disk entry "
           "on a non-DB-primary shard";
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_FALSE(metadataOpt->isSharded());

    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 1);
    ASSERT_EQ(stats.getIntField("versionResolvedAfterRecovery"), 1)
        << "Expected the post-recovery compatibility check to accept UNOWNED + 0-chunks without "
           "falling through to a configTime wait";
    ASSERT_EQ(stats.getIntField("postRecoveryWaitResolvedByConfigTime"), 0);
    ASSERT_EQ(stats.getIntField("postRecoveryWaitResolvedByVersionChange"), 0);
}

TEST_F(AuthoritativeRefreshFixture, UnownedShardVersionCheckAcceptsTrackedWithNoChunksVersion) {
    auto* opCtx = operationContext();

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    ASSERT_OK(onShardVersionMismatch(opCtx, kTestNss, boost::none));

    {
        auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
        ASSERT_TRUE(csr->isUnowned());
    }

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);

    const auto trackedZeroChunksVersion =
        ChunkVersion({OID::gen(), Timestamp(50, 1)}, {0 /* major */, 0 /* minor */});
    ASSERT_DOES_NOT_THROW(
        csr->checkShardVersionOrThrow(opCtx, ShardVersionFactory::make(trackedZeroChunksVersion)));

    ASSERT_DOES_NOT_THROW(
        csr->checkShardVersionOrThrow(opCtx, ShardVersionFactory::make(ChunkVersion::UNTRACKED())));
}

TEST_F(AuthoritativeRefreshFixture, UnownedShardVersionCheckRejectsTrackedVersionWithChunks) {
    auto* opCtx = operationContext();

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    ASSERT_OK(onShardVersionMismatch(opCtx, kTestNss, boost::none));

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    ASSERT_TRUE(csr->isUnowned());

    const auto trackedWithChunksVersion =
        ChunkVersion({OID::gen(), Timestamp(50, 1)}, {1 /* major */, 0 /* minor */});
    ASSERT_THROWS_CODE(
        csr->checkShardVersionOrThrow(opCtx, ShardVersionFactory::make(trackedWithChunksVersion)),
        DBException,
        ErrorCodes::StaleConfig);
}

TEST_F(AuthoritativeRefreshFixture, TrackedCollectionWithNoChunksOnDiskRecoveredCorrectly) {
    auto* opCtx = operationContext();

    const UUID uuid = UUID::gen();
    const OID epoch = OID::gen();
    const Timestamp timestamp(Date_t::now());
    CollectionType collType{kTestNss, epoch, timestamp, Date_t::now(), uuid, kShardKeyPattern};

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);
    {
        DBDirectClient client(opCtx);
        client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace, collType.toBSON());
    }

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    auto status = onShardVersionMismatch(opCtx, kTestNss, boost::none);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_TRUE(metadataOpt->isSharded());
    ASSERT_FALSE(metadataOpt->getShardPlacementVersion().isSet());
}

// Runs recovery on a background thread and pauses it inside the Mode B attempt (Mode A's
// failpoint hit is skipped), invokes `duringPauseFn` on the main thread, then resumes.
template <typename Fn>
void runRecoveryAndInjectInModeB(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 Fn&& duringPauseFn) {
    auto* fp = globalFailPointRegistry().find("hangInRecoverRefreshThread");
    const auto initialTimesEntered = fp->setMode(FailPoint::skip, 1);

    stdx::thread recoveryThread([&] {
        auto client = getGlobalServiceContext()->getService()->makeClient("recoveryThread");
        auto threadOpCtx = client->makeOperationContext();
        ASSERT_OK(FilteringMetadataCache::get(threadOpCtx.get())
                      ->onShardVersionMismatch(
                          threadOpCtx.get(), nss, boost::none /* receivedShardVersion */));
    });

    fp->waitForTimesEntered(initialTimesEntered + 1);
    duringPauseFn();
    fp->setMode(FailPoint::off);
    recoveryThread.join();
}

void setDbPrimaryShardForTest(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const ShardId& shardId,
                              const Timestamp& timestamp) {
    BypassDatabaseMetadataAccess bypass(opCtx,
                                        BypassDatabaseMetadataAccess::Type::kWriteOnly);  // NOLINT
    auto scopedDsr = DatabaseShardingRuntime::acquireExclusive(opCtx, nss.dbName());
    scopedDsr->setDbMetadata(opCtx, DatabaseType{nss.dbName(), shardId, {UUID::gen(), timestamp}});
}

// Non-primary shard, transient primary window (set+clear) keeps the primary at `boost::none` but
// bumps the counter by two: only the counter catches the ABA. Converges to kUnowned after retry.
TEST_F(AuthoritativeRefreshFixture, TransientPrimaryAbaForcesModeBRetry) {
    auto* opCtx = operationContext();

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    runRecoveryAndInjectInModeB(opCtx, kTestNss, [&] {
        BypassDatabaseMetadataAccess bypass(
            opCtx, BypassDatabaseMetadataAccess::Type::kWriteOnly);  // NOLINT
        auto scopedDsr = DatabaseShardingRuntime::acquireExclusive(opCtx, kTestNss.dbName());
        scopedDsr->setDbMetadata(
            opCtx, DatabaseType{kTestNss.dbName(), kMyShardName, {UUID::gen(), Timestamp(10, 1)}});
        scopedDsr->clearDbMetadata(opCtx);
    });

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    ASSERT_TRUE(csr->isUnowned());
    ASSERT_FALSE(csr->getCurrentMetadataIfKnown()->isSharded());

    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 3);  // 1 Mode A + 2 Mode B (one retry).
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 1);
}

// Stable DB primary baseline: no mid-flight mutation, no retry. Converges to kUntracked with
// exactly 1 Mode A + 1 Mode B recoverer.
TEST_F(AuthoritativeRefreshFixture, DbPrimaryShardInstallsUntrackedOnEmptyDisk) {
    auto* opCtx = operationContext();

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    setDbPrimaryShardForTest(opCtx, kTestNss, kMyShardName, Timestamp(1, 1));

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    ASSERT_OK(onShardVersionMismatch(opCtx, kTestNss, boost::none));

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    ASSERT_FALSE(csr->isUnowned());
    ASSERT_FALSE(csr->getCurrentMetadataIfKnown()->isSharded());

    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 2);  // 1 Mode A + 1 Mode B, no retry.
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 1);
}

// A retained DSR entry from the legacy non-authoritative model only says which shard the cached
// metadata names as primary. It does not prove that this shard is the DB primary.
TEST_F(AuthoritativeRefreshFixture, RetainedNonAuthoritativeDsrEntryInstallsUnownedOnEmptyDisk) {
    auto* opCtx = operationContext();

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    {
        auto scopedDsr = DatabaseShardingRuntime::acquireExclusive(opCtx, kTestNss.dbName());
        scopedDsr->setDbInfo_DEPRECATED(
            opCtx,
            DatabaseType{kTestNss.dbName(), ShardId("otherShard"), {UUID::gen(), Timestamp(1, 1)}});
    }

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    ASSERT_OK(onShardVersionMismatch(opCtx, kTestNss, boost::none));

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    ASSERT_TRUE(csr->isUnowned());
    ASSERT_FALSE(csr->getCurrentMetadataIfKnown()->isSharded());

    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 2);  // 1 Mode A + 1 Mode B, no retry.
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 1);
}

// Non-ABA primary identity change: DSR starts empty, a movePrimary during the drain makes this
// shard the new primary. Caught by the simple pre/post compare. Converges to kUntracked after
// retry.
TEST_F(AuthoritativeRefreshFixture, PrimaryChangeDuringRecoveryForcesModeBRetry) {
    auto* opCtx = operationContext();

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    runRecoveryAndInjectInModeB(opCtx, kTestNss, [&] {
        BypassDatabaseMetadataAccess bypass(
            opCtx, BypassDatabaseMetadataAccess::Type::kWriteOnly);  // NOLINT
        auto scopedDsr = DatabaseShardingRuntime::acquireExclusive(opCtx, kTestNss.dbName());
        scopedDsr->setDbMetadata(
            opCtx, DatabaseType{kTestNss.dbName(), kMyShardName, {UUID::gen(), Timestamp(40, 1)}});
    });

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    ASSERT_FALSE(csr->isUnowned());
    ASSERT_FALSE(csr->getCurrentMetadataIfKnown()->isSharded());

    auto stats = getStatistics(opCtx);
    ASSERT_EQ(stats.getIntField("recoverersCreated"), 3);
    ASSERT_EQ(stats.getIntField("diskRecoveriesPerformed"), 1);
}

class RefreshCancellationFixture : public ShardServerTestFixtureWithCatalogCacheLoaderMock {
protected:
    // Cancel incompatible refreshes off-thread (interrupt blocks on drain), then unhang `fp`.
    void interruptRefreshesThenUnhangFailpoint(FailPoint* fp) {
        stdx::thread interruptThread([&] {
            auto client = getGlobalServiceContext()->getService()->makeClient("bgInterrupt");
            auto interruptOpCtx = client->makeOperationContext();
            FilteringMetadataRefreshTracker::get(interruptOpCtx.get())
                ->interruptIncompatibleRefreshes(interruptOpCtx.get());
        });
        sleepmillis(15);  // Best effort wait for the interrupt to happen before unhanging.
        fp->setMode(FailPoint::off);
        interruptThread.join();
    }
};

TEST_F(RefreshCancellationFixture, CancelAuthRefreshRetriesAsNonAuthoritative) {
    auto* opCtx = operationContext();

    // Force the flag on so the bg thread picks the auth path on the first attempt.
    unittest::ServerParameterGuard authoritativeScope("featureFlagAuthoritativeShardsCRUD", true);

    // Cached version matches received, so both the initial auth path (before we cancel it) and the
    // non-auth retry can complete from the local cache.
    setDbPrimaryShardForTest(opCtx, kTestNss, kMyShardName, Timestamp(1, 0));
    const auto receivedDbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 0));

    unittest::LogCaptureGuard logs;

    auto* fp = globalFailPointRegistry().find("hangBeforeAuthoritativeDbVersionMismatchWait");
    const auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);

    stdx::thread t([&] {
        auto bgClient = getGlobalServiceContext()->getService()->makeClient("bgAuth");
        auto bgOpCtx = bgClient->makeOperationContext();
        ASSERT_OK(FilteringMetadataCache::get(bgOpCtx.get())
                      ->onDbVersionMismatch(bgOpCtx.get(), kTestNss.dbName(), receivedDbVersion));
    });

    fp->waitForTimesEntered(initialTimesEntered + 1);

    // Flip the flag before cancelling so the outer retry dispatches to the non-auth path.
    unittest::ServerParameterGuard nonAuthoritativeScope("featureFlagAuthoritativeShardsCRUD",
                                                         false);

    interruptRefreshesThenUnhangFailpoint(fp);
    t.join();

    // The retry-after-cancel log must have fired for the canceled auth attempt.
    ASSERT_EQ(1,
              logs.countBSONContainingSubset(
                  BSON("id" << 12436401 << "attr" << BSON("isAuthoritative" << true))));
}

TEST_F(RefreshCancellationFixture, CancelNonAuthRefreshRetriesAsAuthoritative) {
    auto* opCtx = operationContext();

    // Force the flag off so the bg thread picks the non-auth path on the first attempt.
    unittest::ServerParameterGuard nonAuthoritativeScope("featureFlagAuthoritativeShardsCRUD",
                                                         false);

    // Set a dbVersion lower than receivedDbVersion, to actually run the non-authoritative refresh.
    setDbPrimaryShardForTest(opCtx, kTestNss, kMyShardName, Timestamp(1, 0));
    const auto receivedDbVersion = DatabaseVersion(UUID::gen(), Timestamp(2, 0));

    unittest::LogCaptureGuard logs;

    auto* fp = globalFailPointRegistry().find("hangInRecoverRefreshDbVersionThread");
    const auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);

    stdx::thread t([&] {
        auto bgClient = getGlobalServiceContext()->getService()->makeClient("bgNonAuth");
        auto bgOpCtx = bgClient->makeOperationContext();
        ASSERT_OK(FilteringMetadataCache::get(bgOpCtx.get())
                      ->onDbVersionMismatch(bgOpCtx.get(), kTestNss.dbName(), receivedDbVersion));
    });

    fp->waitForTimesEntered(initialTimesEntered + 1);

    // Bump the cached version so the auth retry's inner loop sees a matching version.
    setDbPrimaryShardForTest(opCtx, kTestNss, kMyShardName, Timestamp(2, 0));

    // Flip the flag before cancelling so the outer retry dispatches to the auth path.
    unittest::ServerParameterGuard authoritativeScope("featureFlagAuthoritativeShardsCRUD", true);

    interruptRefreshesThenUnhangFailpoint(fp);
    t.join();

    // The retry-after-cancel log must have fired for the canceled non-auth attempt.
    ASSERT_EQ(1,
              logs.countBSONContainingSubset(
                  BSON("id" << 12436401 << "attr" << BSON("isAuthoritative" << false))));
}

TEST_F(RefreshCancellationFixture, CancelAuthCollectionRefreshRetriesAsNonAuthoritative) {
    auto* opCtx = operationContext();

    // Force the flag on so the bg thread picks the auth path on the first attempt.
    unittest::ServerParameterGuard authoritativeScope("featureFlagAuthoritativeShardsCRUD", true);

    // Auth CSR with no metadata: forces the authoritative path to recover from disk.
    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    auto sharedMetadata =
        makeShardedMetadataInMemory(opCtx, UUID::gen(), kMyShardName, kMyShardName);
    const auto receivedShardVersion = sharedMetadata.getShardPlacementVersion();

    unittest::LogCaptureGuard logs;

    auto* fp = globalFailPointRegistry().find("hangInRecoverRefreshThread");
    const auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);

    stdx::thread t([&] {
        auto bgClient = getGlobalServiceContext()->getService()->makeClient("bgAuthColl");
        auto bgOpCtx = bgClient->makeOperationContext();
        ASSERT_OK(FilteringMetadataCache::get(bgOpCtx.get())
                      ->onShardVersionMismatch(bgOpCtx.get(), kTestNss, receivedShardVersion));
    });

    fp->waitForTimesEntered(initialTimesEntered + 1);

    // Transition the CSR to non-auth with matching metadata so the retry's non-auth handler
    // observes that the cached version satisfies receivedShardVersion and returns immediately.
    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->setCollectionMetadata(opCtx, sharedMetadata);
        csr->setNonAuthoritative();
    }

    // Flip the flag before cancelling so the outer retry dispatches to the non-auth path.
    unittest::ServerParameterGuard nonAuthoritativeScope("featureFlagAuthoritativeShardsCRUD",
                                                         false);

    interruptRefreshesThenUnhangFailpoint(fp);
    t.join();

    // The retry-after-cancel log must have fired for the canceled auth attempt.
    ASSERT_EQ(1,
              logs.countBSONContainingSubset(
                  BSON("id" << 12436300 << "attr" << BSON("isAuthoritative" << true))));
}

TEST_F(RefreshCancellationFixture, CancelNonAuthCollectionRefreshRetriesAsAuthoritative) {
    auto* opCtx = operationContext();

    // Force the flag off so the bg thread picks the non-auth path on the first attempt.
    unittest::ServerParameterGuard nonAuthoritativeScope("featureFlagAuthoritativeShardsCRUD",
                                                         false);

    auto sharedMetadata =
        makeShardedMetadataInMemory(opCtx, UUID::gen(), kMyShardName, kMyShardName);
    const auto receivedShardVersion = sharedMetadata.getShardPlacementVersion();

    unittest::LogCaptureGuard logs;

    auto* fp = globalFailPointRegistry().find("hangInRecoverRefreshThread");
    const auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);

    stdx::thread t([&] {
        auto bgClient = getGlobalServiceContext()->getService()->makeClient("bgNonAuthColl");
        auto bgOpCtx = bgClient->makeOperationContext();
        ASSERT_OK(FilteringMetadataCache::get(bgOpCtx.get())
                      ->onShardVersionMismatch(bgOpCtx.get(), kTestNss, receivedShardVersion));
    });

    fp->waitForTimesEntered(initialTimesEntered + 1);

    // Promote the CSR to authoritative with matching metadata so the retry's auth path sees the
    // version as already sufficient and returns without performing full disk recovery.
    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->setCollectionMetadata(
            opCtx, sharedMetadata, CollectionShardingRuntime::NoRoutingTableAs::kUntracked);
        csr->setAuthoritative();
    }

    // Flip the flag before cancelling so the outer retry dispatches to the auth path.
    unittest::ServerParameterGuard authoritativeScope("featureFlagAuthoritativeShardsCRUD", true);

    interruptRefreshesThenUnhangFailpoint(fp);
    t.join();

    // The retry-after-cancel log must have fired for the canceled non-auth attempt.
    ASSERT_EQ(1,
              logs.countBSONContainingSubset(
                  BSON("id" << 12436300 << "attr" << BSON("isAuthoritative" << false))));
}

// Test that the refresh path respects the maxTimeMS on the original OperationContext.
TEST_F(RefreshCancellationFixture, MaxTimeMsOnOperationContextInterruptsRefresh) {
    auto* opCtx = operationContext();

    unittest::ServerParameterGuard authoritativeScope("featureFlagAuthoritativeShardsCRUD", true);

    // Force the authoritative path to recover from disk and block.
    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearCollectionMetadata(opCtx);
        csr->setAuthoritative();
    }

    auto sharedMetadata =
        makeShardedMetadataInMemory(opCtx, UUID::gen(), kMyShardName, kMyShardName);
    const auto receivedShardVersion = sharedMetadata.getShardPlacementVersion();

    auto* fp = globalFailPointRegistry().find("hangInRecoverRefreshThread");
    fp->setMode(FailPoint::alwaysOn);

    stdx::thread t([&] {
        auto bgClient = getGlobalServiceContext()->getService()->makeClient("bgMaxTimeMs");
        auto bgOpCtx = bgClient->makeOperationContext();
        // Match how the command dispatch path sets a client's maxTimeMS deadline.
        bgOpCtx->setDeadlineAfterNowBy(Milliseconds(500), ErrorCodes::MaxTimeMSExpired);
        ASSERT_EQ(FilteringMetadataCache::get(bgOpCtx.get())
                      ->onShardVersionMismatch(bgOpCtx.get(), kTestNss, receivedShardVersion),
                  ErrorCodes::MaxTimeMSExpired);
    });

    t.join();
    fp->setMode(FailPoint::off);
}

// Tests that after interrupted refreshes, we block until they have drained.
TEST_F(RefreshCancellationFixture, InterruptIncompatibleRefreshesWaitsForDrain) {
    auto* opCtx = operationContext();
    unittest::ServerParameterGuard nonAuthoritativeScope("featureFlagAuthoritativeShardsCRUD",
                                                         false);
    setDbPrimaryShardForTest(opCtx, kTestNss, kMyShardName, Timestamp(1, 0));
    const auto receivedDbVersion = DatabaseVersion(UUID::gen(), Timestamp(2, 0));

    // Hang a non-authoritative refresh then interrupt it.
    auto* fp = globalFailPointRegistry().find("hangInRecoverRefreshDbVersionThread");
    const auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);
    stdx::thread t([&] {
        auto bgClient = getGlobalServiceContext()->getService()->makeClient("bgNonAuth");
        auto bgOpCtx = bgClient->makeOperationContext();
        ASSERT_OK(FilteringMetadataCache::get(bgOpCtx.get())
                      ->onDbVersionMismatch(bgOpCtx.get(), kTestNss.dbName(), receivedDbVersion));
    });
    fp->waitForTimesEntered(initialTimesEntered + 1);

    setDbPrimaryShardForTest(opCtx, kTestNss, kMyShardName, Timestamp(2, 0));
    unittest::ServerParameterGuard authoritativeScope("featureFlagAuthoritativeShardsCRUD", true);

    AtomicWord<bool> interruptFinished{false};
    stdx::thread interruptThread([&] {
        auto client = getGlobalServiceContext()->getService()->makeClient("bgInterrupt");
        auto interruptOpCtx = client->makeOperationContext();
        FilteringMetadataRefreshTracker::get(interruptOpCtx.get())
            ->interruptIncompatibleRefreshes(interruptOpCtx.get());
        interruptFinished.store(true);
    });

    // Interrupt can't return while the refresh is hung; unhanging it lets the interrupt drain.
    sleepmillis(15);
    ASSERT_FALSE(interruptFinished.load());
    fp->setMode(FailPoint::off);
    interruptThread.join();
    ASSERT_TRUE(interruptFinished.load());

    t.join();
}

}  // namespace
}  // namespace mongo
