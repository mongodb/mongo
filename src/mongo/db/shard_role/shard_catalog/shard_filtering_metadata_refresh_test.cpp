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
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/commit_collection_metadata_locally.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const std::string kShardKey = "_id";
const BSONObj kShardKeyPattern = BSON(kShardKey << 1);
const HostAndPort kSelfShardHostAndPort{"selfShardHost", 12345};

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

    void addSelfShardToRegistry() {
        addRemoteShards({{kMyShardName, kSelfShardHostAndPort}});
    }

    /**
     * Answers the appendOplogNote noop issued by _waitForConfigTimeOrChunkVersionChange when the
     * self-shard is registered as a remote HostAndPort (see addSelfShardToRegistry). Must run on a
     * different thread than the code that calls runCommand (e.g. pair with launchAsync).
     */
    void expectAppendOplogNoteNoopFromSelfShard() {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kSelfShardHostAndPort, request.target);
            ASSERT_EQ(DatabaseName::kAdmin, request.dbname);
            ASSERT_TRUE(request.cmdObj.hasField("appendOplogNote"));
            return BSON("ok" << 1);
        });
    }

    /**
     * Runs onCollectionPlacementVersionMismatch on an async task and serves the self-shard
     * appendOplogNote from the mock network (required whenever recovery reaches the configTime /
     * chunk-version wait after addSelfShardToRegistry).
     */
    Status onShardVersionMismatchExpectSelfShardNoop(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     boost::optional<ChunkVersion> received) {
        auto future = launchAsync([&] { return onShardVersionMismatch(opCtx, nss, received); });
        expectAppendOplogNoteNoopFromSelfShard();
        return future.default_timed_get();
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
        return FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
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
};

TEST_F(AuthoritativeRefreshFixture, UntrackedIsCorrectlyRecoveredFromDisk) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto untrackedNss =
        NamespaceString::createNamespaceString_forTest("TestDB", "UntrackedColl");

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, untrackedNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
    }

    auto status = onShardVersionMismatch(opCtx, untrackedNss, boost::none);
    ASSERT_OK(status);
    auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, untrackedNss);
    ASSERT_TRUE(csr->getCurrentMetadataIfKnown());
    ASSERT_FALSE(csr->getCurrentMetadataIfKnown()->isSharded());
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

    auto status = onShardVersionMismatch(opCtx, kTestNss, boost::none);
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

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    auto matchingVersion = chunks.back().getVersion();

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
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

TEST_F(AuthoritativeRefreshFixture,
       UntrackedRouterVersionWithKnownTrackedMetadataSkipsSecondDiskRecovery) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
    }

    ASSERT_OK(onShardVersionMismatch(opCtx, kTestNss, boost::none));

    auto statsAfterFirst = getStatistics(opCtx);
    ASSERT_EQ(statsAfterFirst.getIntField("diskRecoveriesPerformed"), 1);
    ASSERT_EQ(statsAfterFirst.getIntField("recoverersCreated"), 1);

    addSelfShardToRegistry();
    ASSERT_OK(
        onShardVersionMismatchExpectSelfShardNoop(opCtx, kTestNss, ChunkVersion::UNTRACKED()));

    auto statsAfterSecond = getStatistics(opCtx);
    ASSERT_EQ(statsAfterSecond.getIntField("diskRecoveriesPerformed"), 1)
        << "Second placement mismatch should not re-run disk recovery when CSS already has "
           "metadata";
    ASSERT_EQ(statsAfterSecond.getIntField("recoverersCreated"), 1);
}

TEST_F(AuthoritativeRefreshFixture,
       IgnoredReceivedVersionResolvesAfterRecoveryWithoutPostRecoveryWait) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
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
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    auto currentVersion = chunks.back().getVersion();
    auto higherVersion = currentVersion;
    higherVersion.incMajor();

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
    }

    addSelfShardToRegistry();
    auto status = onShardVersionMismatchExpectSelfShardNoop(opCtx, kTestNss, higherVersion);
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
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    auto dummyVersion = ChunkVersion({OID::gen(), Timestamp(50, 1)}, {1, 0});
    addSelfShardToRegistry();

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
        csr->enterCriticalSectionCatchUpPhase(opCtx, BSONObj());
        csr->enterCriticalSectionCommitPhase(opCtx, BSONObj());
    }

    auto* fp = globalFailPointRegistry().find("hangBeforePlacementVersionCriticalSectionWait");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);

    stdx::thread noopResponder([&] { expectAppendOplogNoteNoopFromSelfShard(); });

    stdx::thread recoveryThread([&] {
        auto bgClient = getGlobalServiceContext()->getService()->makeClient("bgFlip");
        auto bgOpCtx = bgClient->makeOperationContext();
        auto bgStatus = onShardVersionMismatch(bgOpCtx.get(), kTestNss, dummyVersion);
        ASSERT_OK(bgStatus);
    });

    fp->waitForTimesEntered(initialTimesEntered + 1);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->setFilteringMetadata_nonAuthoritative(opCtx, CollectionMetadata::UNTRACKED());
        csr->exitCriticalSection(opCtx, BSONObj());
    }

    advanceCommittedSnapshot(opCtx);

    fp->setMode(FailPoint::off);

    recoveryThread.join();
    noopResponder.join();

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    ASSERT_EQ(csr->getAuthoritativeState(),
              CollectionShardingRuntime::AuthoritativeState::kNonAuthoritative);
}

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

TEST_F(AuthoritativeRefreshFixture, RecoveryCreatesExactlyOneRecoverer) {
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
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 3, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
    }

    auto status = onShardVersionMismatch(opCtx, kTestNss, boost::none);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    ASSERT_TRUE(csr->getCurrentMetadataIfKnown().has_value());
    ASSERT_FALSE(csr->getCollectionCacheRecoverer());
}

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
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 1, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
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
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 100, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
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
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
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
        csr->clearFilteringMetadata_authoritative(opCtx);
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
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 5, kMyShardName);
    populateDiskCatalog(opCtx, collType, chunks);

    auto matchingVersion = chunks.back().getVersion();

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
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
        csr->clearFilteringMetadata_authoritative(opCtx);
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
        csr->setFilteringMetadata_authoritative(opCtx, externalMetadata);
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

TEST_F(AuthoritativeRefreshFixture, TrackedCollectionWithNoChunksOnDiskRecoveredCorrectly) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagShardAuthoritativeCollMetadata",
                                                     true);
    auto* opCtx = operationContext();

    const UUID uuid = UUID::gen();
    const OID epoch = OID::gen();
    const Timestamp timestamp(Date_t::now());
    CollectionType collType{kTestNss, epoch, timestamp, Date_t::now(), uuid, kShardKeyPattern};

    auto keyPattern = KeyPattern(kShardKeyPattern);
    auto range = ChunkRange(keyPattern.globalMin(), keyPattern.globalMax());
    ChunkType placeholder(uuid,
                          std::move(range),
                          ChunkVersion({epoch, timestamp}, {1, 0}),
                          shard_catalog_commit::kChunklessPlaceholderShardId);
    placeholder.setName(OID::gen());

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);
    {
        DBDirectClient client(opCtx);
        client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace, collType.toBSON());
        client.insert(NamespaceString::kConfigShardCatalogChunksNamespace,
                      placeholder.toConfigBSON());
    }

    {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        csr->clearFilteringMetadata_authoritative(opCtx);
    }

    auto status = onShardVersionMismatch(opCtx, kTestNss, boost::none);
    ASSERT_OK(status);

    auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
    auto metadataOpt = csr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadataOpt.has_value());
    ASSERT_TRUE(metadataOpt->isSharded());
    ASSERT_FALSE(metadataOpt->getShardPlacementVersion().isSet());
}

}  // namespace
}  // namespace mongo
