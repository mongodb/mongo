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

#include "mongo/db/shard_role/shard_catalog/collection_cache_recoverer.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/read_concern_mongod_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const std::string kShardKey = "_id";
const BSONObj kShardKeyPattern = BSON(kShardKey << 1);
// Every test in this file places chunks on a single shard.
const ShardId kShard{"0"};

// Builds a shard-key bound at the given value, e.g. {_id: 150}.
BSONObj shardKey(int value) {
    return BSON(kShardKey << value);
}

std::pair<CollectionType, std::vector<ChunkType>> makeShardedMetadataForDisk(
    OperationContext* opCtx, int nChunks, ShardId shardId) {
    const UUID uuid = UUID::gen();
    const OID epoch = OID::gen();
    const Timestamp timestamp(Date_t::now());

    CollectionType collType{kTestNss, epoch, timestamp, Date_t::now(), uuid, kShardKeyPattern};

    std::vector<ChunkType> chunks;
    auto chunkVersion = ChunkVersion({epoch, timestamp}, {1, 0});
    for (int i = 0; i < nChunks; i++) {
        auto min = i == 0 ? BSON(kShardKey << MINKEY) : shardKey(i * 100);
        auto max = i == (nChunks - 1) ? BSON(kShardKey << MAXKEY) : shardKey((i + 1) * 100);
        auto range = ChunkRange(min, max);
        auto& chunkInserted = chunks.emplace_back(uuid, std::move(range), chunkVersion, shardId);
        chunkInserted.setName(OID::gen());
        chunkVersion.incMajor();
    }

    return {std::move(collType), std::move(chunks)};
}

// Packs the changed chunks into a single delta oplog entry. Used to feed changed chunks to the
// recoverer so they get merged onto the recovered routing table.
CollectionShardingStateDeltaOplogEntry makeDeltaEntry(const std::vector<ChunkType>& chunks) {
    std::vector<BSONObj> changedChunks;
    changedChunks.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        changedChunks.push_back(chunk.toConfigBSON());
    }
    return CollectionShardingStateDeltaOplogEntry{std::string(kTestNss.coll()),
                                                  std::move(changedChunks)};
}

// Builds a named chunk covering [min, max) owned by 'shard' at the given version.
ChunkType makeChunk(const UUID& uuid,
                    BSONObj min,
                    BSONObj max,
                    ChunkVersion version,
                    const ShardId& shard = kShard) {
    ChunkType chunk{uuid, ChunkRange(std::move(min), std::move(max)), std::move(version), shard};
    chunk.setName(OID::gen());
    return chunk;
}

// Splits 'chunk' at 'splitPoint' into [min, splitPoint) and [splitPoint, max), stamping the two
// halves with 'lowVersion' and 'highVersion'. Models the chunks a split would write.
std::vector<ChunkType> splitChunk(const ChunkType& chunk,
                                  int splitPoint,
                                  ChunkVersion lowVersion,
                                  ChunkVersion highVersion) {
    const auto& uuid = chunk.getCollectionUUID();
    return {makeChunk(uuid, chunk.getMin(), shardKey(splitPoint), std::move(lowVersion)),
            makeChunk(uuid, shardKey(splitPoint), chunk.getMax(), std::move(highVersion))};
}

// Asserts the routing table maps 'key' to a chunk with the expected bounds, owning shard and
// version, proving the delta chunk was really applied with its range.
void assertChunkAt(const CollectionMetadata& metadata,
                   const BSONObj& key,
                   const BSONObj& expectedMin,
                   const BSONObj& expectedMax,
                   const ShardId& expectedShard,
                   const ChunkVersion& expectedVersion) {
    const auto chunk = metadata.getChunkManager()->findIntersectingChunkWithSimpleCollation(key);
    ASSERT_BSONOBJ_EQ(chunk.getMin(), expectedMin);
    ASSERT_BSONOBJ_EQ(chunk.getMax(), expectedMax);
    ASSERT_EQ(chunk.getShardId(), expectedShard);
    ASSERT_EQ(chunk.getLastmod(), expectedVersion);
}

class RecovererFixture : public ShardServerTestFixture {
protected:
    void seedShardCatalogOnDisk(OperationContext* opCtx,
                                const CollectionType& collType,
                                const std::vector<ChunkType>& chunks) {
        createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
        createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

        DBDirectClient client(opCtx);
        client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace, collType.toBSON());
        for (const auto& chunk : chunks) {
            client.insert(NamespaceString::kConfigShardCatalogChunksNamespace,
                          chunk.toConfigBSON());
        }
    }

    // Returns the timestamp the next recovery round will read at, i.e. the timestamp an oplog
    // entry must meet or exceed to be enqueued rather than dropped.
    Timestamp lastWrittenTimestamp() {
        return repl::ReplicationCoordinator::get(operationContext())
            ->getMyLastWrittenOpTime()
            .getTimestamp();
    }

    void setUp() override {
        ShardServerTestFixture::setUp();
        _executor = std::make_shared<ThreadPool>([] {
            ThreadPool::Options options;
            options.poolName = "TestCSSRecoveryPool";
            options.minThreads = 1;
            options.maxThreads = 1;
            return options;
        }());
        _executor->startup();
    }

    void tearDown() override {
        ShardServerTestFixture::tearDown();
        _executor->shutdown();
    }

    ExecutorPtr getExecutor() {
        return _executor;
    }

private:
    std::shared_ptr<ThreadPool> _executor;
};

}  // namespace

TEST_F(RecovererFixture, CacheRecovererCanRecoverFromDisk) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionCacheRecoverer recoverer{kTestNss, CancellationToken::uncancelable()};

    auto roundId = recoverer.start(operationContext(), getExecutor());
    ASSERT_OK(recoverer.waitForInitialPass(operationContext(), roundId));
    auto collMetadata = recoverer.drainAndApply(operationContext(), roundId);

    ASSERT_TRUE(collMetadata);

    const auto shardVersionExpected = chunks.back().getVersion();

    ASSERT_EQ(collMetadata->getCollPlacementVersion(), shardVersionExpected);
}

TEST_F(RecovererFixture, QueryableBackupModeRecoversFromLocalCatalogWithoutExecutor) {
    OperationContext* opCtx = operationContext();
    const auto originalQueryableBackupMode = storageGlobalParams.queryableBackupMode;
    ON_BLOCK_EXIT([&] { storageGlobalParams.queryableBackupMode = originalQueryableBackupMode; });
    storageGlobalParams.queryableBackupMode = true;

    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionCacheRecoverer recoverer{kTestNss, CancellationToken::uncancelable()};

    auto roundId = recoverer.start(opCtx, nullptr /* executor */);
    ASSERT_OK(recoverer.waitForInitialPass(opCtx, roundId));
    auto collMetadata = recoverer.drainAndApply(opCtx, roundId);

    ASSERT_TRUE(collMetadata);
    ASSERT_TRUE(collMetadata->isSharded());
    ASSERT_EQ(collMetadata->getChunkManager()->numChunks(), numChunks);
    ASSERT_EQ(collMetadata->getCollPlacementVersion(), chunks.back().getVersion());
    ASSERT_TRUE(repl::ReadConcernArgs::get(opCtx).isEmpty());
}

TEST_F(RecovererFixture,
       TestingSnapshotBehaviorInIsolationRecoversFromLocalCatalogWithoutExecutor) {
    OperationContext* opCtx = operationContext();
    const auto originalTestingSnapshotBehaviorInIsolation = gTestingSnapshotBehaviorInIsolation;
    ON_BLOCK_EXIT(
        [&] { gTestingSnapshotBehaviorInIsolation = originalTestingSnapshotBehaviorInIsolation; });
    gTestingSnapshotBehaviorInIsolation = true;

    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionCacheRecoverer recoverer{kTestNss, CancellationToken::uncancelable()};

    auto roundId = recoverer.start(opCtx, nullptr /* executor */);
    ASSERT_OK(recoverer.waitForInitialPass(opCtx, roundId));
    auto collMetadata = recoverer.drainAndApply(opCtx, roundId);

    ASSERT_TRUE(collMetadata);
    ASSERT_TRUE(collMetadata->isSharded());
    ASSERT_EQ(collMetadata->getChunkManager()->numChunks(), numChunks);
    ASSERT_EQ(collMetadata->getCollPlacementVersion(), chunks.back().getVersion());
    ASSERT_TRUE(repl::ReadConcernArgs::get(opCtx).isEmpty());
}

TEST_F(RecovererFixture, CacheRecovererRecoversAllChunksRegardlessOfDiskInsertionOrder) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    {
        DBDirectClient client(opCtx);
        client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace, collType.toBSON());
    }

    // Insert the chunks in reverse 'lastmod' order on disk. The recovery aggregation must still
    // return the full, version-ordered routing table independently of the on-disk insertion order.
    for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) {
        DBDirectClient client(opCtx);
        client.insert(NamespaceString::kConfigShardCatalogChunksNamespace, it->toConfigBSON());
    }

    CollectionCacheRecoverer recoverer{kTestNss, CancellationToken::uncancelable()};

    auto roundId = recoverer.start(operationContext(), getExecutor());
    ASSERT_OK(recoverer.waitForInitialPass(operationContext(), roundId));
    auto collMetadata = recoverer.drainAndApply(operationContext(), roundId);

    ASSERT_TRUE(collMetadata);
    ASSERT_TRUE(collMetadata->isSharded());
    ASSERT_EQ(collMetadata->getChunkManager()->numChunks(), numChunks);
    ASSERT_EQ(collMetadata->getCollPlacementVersion(), chunks.back().getVersion());
}

TEST_F(RecovererFixture, CacheRecovererCanRecoverFromDiskAnUntrackedCollection) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    CollectionCacheRecoverer recoverer{kTestNss, CancellationToken::uncancelable()};

    auto roundId = recoverer.start(operationContext(), getExecutor());
    ASSERT_OK(recoverer.waitForInitialPass(operationContext(), roundId));
    auto collMetadata = recoverer.drainAndApply(operationContext(), roundId);

    ASSERT_TRUE(collMetadata);

    ASSERT_FALSE(collMetadata->isSharded());
}

TEST_F(RecovererFixture, CacheRecovererAppliesOplogChanges) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    auto collMetadata = [&] {
        CollectionCacheRecoverer recoverer{kTestNss, CancellationToken::uncancelable()};

        auto roundId = recoverer.start(operationContext(), getExecutor());
        ASSERT_OK(recoverer.waitForInitialPass(operationContext(), roundId));
        return recoverer.drainAndApply(operationContext(), roundId);
    }();

    ASSERT_TRUE(collMetadata);

    auto changedChunk = chunks.front();
    auto changedChunkVersion = chunks.back().getVersion();
    changedChunkVersion.incMajor();
    changedChunk.setVersion(changedChunkVersion);

    CollectionCacheRecoverer recoverer{
        kTestNss, CancellationToken::uncancelable(), std::move(*collMetadata)};
    auto roundId = recoverer.start(operationContext(), getExecutor());
    recoverer.onOplogEntry(lastWrittenTimestamp() + 1, makeDeltaEntry({changedChunk}));
    collMetadata = recoverer.drainAndApply(operationContext(), roundId);

    ASSERT_TRUE(collMetadata);
    ASSERT_EQ(collMetadata->getCollPlacementVersion(), changedChunkVersion);
}

// A delta that arrives during recovery is applied on top of the metadata recovered from disk.
TEST_F(RecovererFixture, CacheRecovererMergesDeltaOntoDiskRecoveredBase) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionCacheRecoverer recoverer{kTestNss, CancellationToken::uncancelable()};
    auto roundId = recoverer.start(operationContext(), getExecutor());

    // The delta splits the recovered chunk [100, 200) into [100, 150) and [150, 200). It is
    // enqueued before the disk pass produces the base, so it must be replayed on top of the
    // disk-recovered routing table during the drain.
    auto nextVersion = [v = chunks.back().getVersion()]() mutable {
        v.incMajor();
        return v;
    };
    const auto firstHalfVersion = nextVersion();
    const auto secondHalfVersion = nextVersion();
    recoverer.onOplogEntry(
        lastWrittenTimestamp() + 1,
        makeDeltaEntry(splitChunk(chunks[1], 150, firstHalfVersion, secondHalfVersion)));

    ASSERT_OK(recoverer.waitForInitialPass(operationContext(), roundId));
    auto collMetadata = recoverer.drainAndApply(operationContext(), roundId);

    ASSERT_TRUE(collMetadata);
    ASSERT_EQ(collMetadata->getCollPlacementVersion(), secondHalfVersion);

    // The two new sub-ranges replaced the original [100, 200) chunk.
    assertChunkAt(
        *collMetadata, shardKey(120), shardKey(100), shardKey(150), kShard, firstHalfVersion);
    assertChunkAt(
        *collMetadata, shardKey(170), shardKey(150), shardKey(200), kShard, secondHalfVersion);
}

// Several queued deltas are applied, each introducing brand new chunks.
TEST_F(RecovererFixture, CacheRecovererDrainsMultipleDeltas) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionCacheRecoverer recoverer{kTestNss, CancellationToken::uncancelable()};
    auto roundId = recoverer.start(operationContext(), getExecutor());

    // Two deltas, each carrying chunks that do not exist on disk: they split recovered chunks at
    // new boundaries (50 and 150). The drain applies them front-to-back, so all four new
    // sub-ranges must be present and the final placement version must equal the latest delta's.
    auto nextVersion = [v = chunks.back().getVersion()]() mutable {
        v.incMajor();
        return v;
    };
    const auto frontLowVersion = nextVersion();
    const auto frontHighVersion = nextVersion();
    const auto secondLowVersion = nextVersion();
    const auto secondHighVersion = nextVersion();

    // Delta 1 splits the front chunk [MinKey, 100) into [MinKey, 50) and [50, 100).
    auto frontSplit = splitChunk(chunks.front(), 50, frontLowVersion, frontHighVersion);
    // Delta 2 splits the [100, 200) chunk into [100, 150) and [150, 200).
    auto secondSplit = splitChunk(chunks[1], 150, secondLowVersion, secondHighVersion);

    const auto baseTs = lastWrittenTimestamp();
    recoverer.onOplogEntry(baseTs + 1, makeDeltaEntry(frontSplit));
    recoverer.onOplogEntry(baseTs + 2, makeDeltaEntry(secondSplit));

    ASSERT_OK(recoverer.waitForInitialPass(operationContext(), roundId));
    auto collMetadata = recoverer.drainAndApply(operationContext(), roundId);

    ASSERT_TRUE(collMetadata);
    ASSERT_EQ(collMetadata->getCollPlacementVersion(), secondHighVersion);

    // All four new sub-ranges from both deltas are present, each with its own version.
    assertChunkAt(*collMetadata,
                  shardKey(25),
                  chunks.front().getMin(),
                  shardKey(50),
                  kShard,
                  frontLowVersion);
    assertChunkAt(
        *collMetadata, shardKey(75), shardKey(50), shardKey(100), kShard, frontHighVersion);
    assertChunkAt(
        *collMetadata, shardKey(120), shardKey(100), shardKey(150), kShard, secondLowVersion);
    assertChunkAt(
        *collMetadata, shardKey(170), shardKey(150), shardKey(200), kShard, secondHighVersion);
}

// An invalidate queued after a delta forces a new recovery round and drops the delta.
TEST_F(RecovererFixture, CacheRecovererRestartsWhenDeltaFollowedByInvalidate) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionCacheRecoverer recoverer{kTestNss, CancellationToken::uncancelable()};
    auto roundId = recoverer.start(operationContext(), getExecutor());
    ASSERT_OK(recoverer.waitForInitialPass(operationContext(), roundId));

    // A delta carrying new chunks (a split of [MinKey, 100) into [MinKey, 50) and [50, 100))
    // followed by an invalidate: the drain applies the delta, then the invalidate forces a fresh
    // recovery round and discards the in-progress result.
    auto lowVersion = chunks.back().getVersion();
    lowVersion.incMajor();
    auto highVersion = lowVersion;
    highVersion.incMajor();
    auto frontSplit = splitChunk(chunks.front(), 50, lowVersion, highVersion);
    const auto baseTs = lastWrittenTimestamp();
    recoverer.onOplogEntry(baseTs + 1, makeDeltaEntry(frontSplit));
    recoverer.onOplogEntry(baseTs + 2,
                           InvalidateCollectionMetadataOplogEntry{std::string(kTestNss.coll())});

    auto collMetadata = recoverer.drainAndApply(operationContext(), roundId);
    ASSERT_FALSE(collMetadata);

    // The next round re-reads disk and is unaffected by the discarded delta.
    roundId = recoverer.start(operationContext(), getExecutor());
    ASSERT_OK(recoverer.waitForInitialPass(operationContext(), roundId));
    collMetadata = recoverer.drainAndApply(operationContext(), roundId);

    ASSERT_TRUE(collMetadata);
    ASSERT_EQ(collMetadata->getCollPlacementVersion(), chunks.back().getVersion());
}

// A delta older than the recovered version is ignored and leaves the metadata unchanged.
TEST_F(RecovererFixture, CacheRecovererSkipsStaleDelta) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionCacheRecoverer recoverer{kTestNss, CancellationToken::uncancelable()};
    auto roundId = recoverer.start(operationContext(), getExecutor());

    // A delta whose version is below the recovered placement version is already reflected in the
    // recovered base, so it is idempotently ignored and leaves the routing table untouched.
    recoverer.onOplogEntry(lastWrittenTimestamp() + 1, makeDeltaEntry({chunks.front()}));

    ASSERT_OK(recoverer.waitForInitialPass(operationContext(), roundId));
    auto collMetadata = recoverer.drainAndApply(operationContext(), roundId);

    ASSERT_TRUE(collMetadata);
    ASSERT_EQ(collMetadata->getCollPlacementVersion(), chunks.back().getVersion());
    ASSERT_EQ(collMetadata->getChunkManager()->numChunks(), numChunks);

    // The front chunk keeps its original range and original version: the stale delta changed
    // nothing.
    assertChunkAt(*collMetadata,
                  shardKey(50),
                  chunks.front().getMin(),
                  chunks.front().getMax(),
                  kShard,
                  chunks.front().getVersion());
}

// A delta with a timestamp before the recovery snapshot is dropped instead of being queued.
TEST_F(RecovererFixture, CacheRecovererDropsDeltaBeforeRecoveryTimestamp) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionCacheRecoverer recoverer{kTestNss, CancellationToken::uncancelable()};
    auto roundId = recoverer.start(operationContext(), getExecutor());

    // An entry whose timestamp predates the recovery snapshot is already captured by the disk read,
    // so it must be dropped rather than enqueued.
    auto deltaVersion = chunks.back().getVersion();
    deltaVersion.incMajor();
    auto bumpedChunk = chunks.front();
    bumpedChunk.setVersion(deltaVersion);
    recoverer.onOplogEntry(Timestamp(1, 0), makeDeltaEntry({bumpedChunk}));

    ASSERT_OK(recoverer.waitForInitialPass(operationContext(), roundId));
    auto collMetadata = recoverer.drainAndApply(operationContext(), roundId);

    ASSERT_TRUE(collMetadata);
    ASSERT_EQ(collMetadata->getCollPlacementVersion(), chunks.back().getVersion());

    // The front chunk keeps its original range and original version: the dropped delta never
    // applied.
    assertChunkAt(*collMetadata,
                  shardKey(50),
                  chunks.front().getMin(),
                  chunks.front().getMax(),
                  kShard,
                  chunks.front().getVersion());
}

TEST_F(RecovererFixture, CacheRecovererCanRecoverFromDiskWithConcurrentOplogEntries) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionCacheRecoverer recoverer{kTestNss, CancellationToken::uncancelable()};

    auto collMetadata = [&]() {
        auto roundId = recoverer.start(operationContext(), getExecutor());
        ASSERT_OK(recoverer.waitForInitialPass(operationContext(), roundId));
        auto recoveryTimestamp =
            repl::ReplicationCoordinator::get(operationContext())->getMyLastWrittenOpTime();
        // We now add an oplog entry that invalidates the previous recovery.
        recoverer.onOplogEntry(
            recoveryTimestamp.getTimestamp() + 1,
            InvalidateCollectionMetadataOplogEntry{std::string(kTestNss.coll())});
        auto collMetadata = recoverer.drainAndApply(operationContext(), roundId);
        // This should've encountered an invalidate entry which triggers a new round of wait +
        // drain.
        ASSERT_FALSE(collMetadata);
        roundId = recoverer.start(operationContext(), getExecutor());
        ASSERT_OK(recoverer.waitForInitialPass(operationContext(), roundId));
        collMetadata = recoverer.drainAndApply(operationContext(), roundId);
        // Recovery should've happened by now and returned the final state.
        ASSERT_TRUE(collMetadata);
        return collMetadata;
    }();

    const auto shardVersionExpected = chunks.back().getVersion();

    ASSERT_EQ(collMetadata->getCollPlacementVersion(), shardVersionExpected);
}

TEST_F(RecovererFixture, CacheRecovererBubblesUpCachePressureErrors) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    FailPointEnableBlock intermittentFailure{"WTWriteConflictExceptionForReads"};

    CollectionCacheRecoverer recoverer{kTestNss, CancellationToken::uncancelable()};

    auto roundId = recoverer.start(operationContext(), getExecutor());
    auto status = recoverer.waitForInitialPass(operationContext(), roundId);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.code(), ErrorCodes::WriteConflict);
}

TEST_F(RecovererFixture, CacheRecovererBubblesUpDiskReadingFailure) {
    OperationContext* opCtx = operationContext();

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    {
        DBDirectClient client(opCtx);
        client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace,
                      BSON("_id" << kTestNss.toStringForErrorMsg() << "made_up" << true));
    }

    {
        CollectionCacheRecoverer recoverer{kTestNss, CancellationToken::uncancelable()};

        // The CollectionType is parsed via an IDL parser. So it should throw an IDL failure.
        auto roundId = recoverer.start(operationContext(), getExecutor());
        auto status = recoverer.waitForInitialPass(operationContext(), roundId);
        ASSERT_NOT_OK(status);
        ASSERT_EQ(status.code(), ErrorCodes::IDLFailedToParse);
    }

    int numChunks = 20;
    const auto [collType, _] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);

    {
        DBDirectClient client(opCtx);
        client.remove(NamespaceString::kConfigShardCatalogCollectionsNamespace,
                      BSON("_id" << kTestNss.toStringForErrorMsg()));
        client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace, collType.toBSON());
    }

    DBDirectClient client(opCtx);
    client.insert(NamespaceString::kConfigShardCatalogChunksNamespace,
                  BSON("uuid" << collType.getUuid() << "lastmod" << "Invalid value"));

    {
        CollectionCacheRecoverer recoverer{kTestNss, CancellationToken::uncancelable()};

        // The ChunkType uses a custom parser that returns a different family of errors compared to
        // the CollectionType. Let's make sure that's the case.
        auto roundId = recoverer.start(operationContext(), getExecutor());
        auto status = recoverer.waitForInitialPass(operationContext(), roundId);
        ASSERT_NOT_OK(status);
        ASSERT_EQ(status.code(), ErrorCodes::NoSuchKey);
    }
}

TEST_F(RecovererFixture, CacheRecovererFailsDueToDifferentRoundId) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionCacheRecoverer recoverer{kTestNss, CancellationToken::uncancelable()};

    auto roundId = recoverer.start(operationContext(), getExecutor());
    ASSERT_OK(recoverer.waitForInitialPass(operationContext(), roundId));
    // We now add an oplog entry that invalidates the previous recovery.
    auto recoveryTimestamp =
        repl::ReplicationCoordinator::get(operationContext())->getMyLastWrittenOpTime();
    recoverer.onOplogEntry(recoveryTimestamp.getTimestamp() + 1,
                           InvalidateCollectionMetadataOplogEntry{std::string(kTestNss.coll())});
    auto collMetadata = recoverer.drainAndApply(operationContext(), roundId);
    // This should've encountered an invalidate entry which triggers a new round of wait +
    // drain.
    ASSERT_FALSE(collMetadata);

    // If a separate thread calls with the previous round then it should fail.
    ASSERT_EQ(recoverer.waitForInitialPass(operationContext(), roundId).code(),
              ErrorCodes::AtomicityFailure);
    ASSERT_FALSE(recoverer.drainAndApply(operationContext(), roundId));
}

}  // namespace mongo
