// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/collection_metadata_synchronizer.h"

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

// Packs the changed chunks into a single UpdateCollectionMetadata oplog entry. Used to feed changed
// chunks to the synchronizer so they get merged onto the recovered routing table.
UpdateCollectionMetadataOplogEntry makeDeltaEntry(const std::vector<ChunkType>& chunks) {
    std::vector<BSONObj> changedChunks;
    changedChunks.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        changedChunks.push_back(chunk.toConfigBSON());
    }
    return UpdateCollectionMetadataOplogEntry{std::string(kTestNss.coll()),
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

class MetadataSynchronizerFixture : public ShardServerTestFixture {
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

TEST_F(MetadataSynchronizerFixture, MetadataSynchronizerCanRecoverFromDisk) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionMetadataSynchronizer synchronizer{kTestNss, CancellationToken::uncancelable()};

    synchronizer.start(operationContext(), getExecutor());
    ASSERT_OK(synchronizer.getMetadataFuture().getNoThrow(operationContext()));
    auto collMetadata = synchronizer.drainAndApply(operationContext());

    ASSERT_TRUE(collMetadata);

    const auto shardVersionExpected = chunks.back().getVersion();

    ASSERT_EQ(collMetadata->getCollPlacementVersion(), shardVersionExpected);
}

TEST_F(MetadataSynchronizerFixture, QueryableBackupModeRecoversFromLocalCatalogWithoutExecutor) {
    OperationContext* opCtx = operationContext();
    const auto originalQueryableBackupMode = storageGlobalParams.queryableBackupMode;
    ON_BLOCK_EXIT([&] { storageGlobalParams.queryableBackupMode = originalQueryableBackupMode; });
    storageGlobalParams.queryableBackupMode = true;

    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionMetadataSynchronizer synchronizer{kTestNss, CancellationToken::uncancelable()};

    synchronizer.start(opCtx, nullptr /* executor */);
    ASSERT_OK(synchronizer.getMetadataFuture().getNoThrow(opCtx));
    auto collMetadata = synchronizer.drainAndApply(opCtx);

    ASSERT_TRUE(collMetadata);
    ASSERT_TRUE(collMetadata->isSharded());
    ASSERT_EQ(collMetadata->getChunkManager()->numChunks(), numChunks);
    ASSERT_EQ(collMetadata->getCollPlacementVersion(), chunks.back().getVersion());
    ASSERT_TRUE(repl::ReadConcernArgs::get(opCtx).isEmpty());
}

TEST_F(MetadataSynchronizerFixture,
       TestingSnapshotBehaviorInIsolationRecoversFromLocalCatalogWithoutExecutor) {
    OperationContext* opCtx = operationContext();
    const auto originalTestingSnapshotBehaviorInIsolation = gTestingSnapshotBehaviorInIsolation;
    ON_BLOCK_EXIT(
        [&] { gTestingSnapshotBehaviorInIsolation = originalTestingSnapshotBehaviorInIsolation; });
    gTestingSnapshotBehaviorInIsolation = true;

    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionMetadataSynchronizer synchronizer{kTestNss, CancellationToken::uncancelable()};

    synchronizer.start(opCtx, nullptr /* executor */);
    ASSERT_OK(synchronizer.getMetadataFuture().getNoThrow(opCtx));
    auto collMetadata = synchronizer.drainAndApply(opCtx);

    ASSERT_TRUE(collMetadata);
    ASSERT_TRUE(collMetadata->isSharded());
    ASSERT_EQ(collMetadata->getChunkManager()->numChunks(), numChunks);
    ASSERT_EQ(collMetadata->getCollPlacementVersion(), chunks.back().getVersion());
    ASSERT_TRUE(repl::ReadConcernArgs::get(opCtx).isEmpty());
}

TEST_F(MetadataSynchronizerFixture,
       MetadataSynchronizerRecoversAllChunksRegardlessOfDiskInsertionOrder) {
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

    CollectionMetadataSynchronizer synchronizer{kTestNss, CancellationToken::uncancelable()};

    synchronizer.start(operationContext(), getExecutor());
    ASSERT_OK(synchronizer.getMetadataFuture().getNoThrow(operationContext()));
    auto collMetadata = synchronizer.drainAndApply(operationContext());

    ASSERT_TRUE(collMetadata);
    ASSERT_TRUE(collMetadata->isSharded());
    ASSERT_EQ(collMetadata->getChunkManager()->numChunks(), numChunks);
    ASSERT_EQ(collMetadata->getCollPlacementVersion(), chunks.back().getVersion());
}

TEST_F(MetadataSynchronizerFixture, MetadataSynchronizerCanRecoverFromDiskAnUntrackedCollection) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    CollectionMetadataSynchronizer synchronizer{kTestNss, CancellationToken::uncancelable()};

    synchronizer.start(operationContext(), getExecutor());
    ASSERT_OK(synchronizer.getMetadataFuture().getNoThrow(operationContext()));
    auto collMetadata = synchronizer.drainAndApply(operationContext());

    ASSERT_TRUE(collMetadata);

    ASSERT_FALSE(collMetadata->isSharded());
}

// A delta that arrives during recovery is applied on top of the metadata recovered from disk.
TEST_F(MetadataSynchronizerFixture, MetadataSynchronizerMergesDeltaOntoDiskRecoveredBase) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionMetadataSynchronizer synchronizer{kTestNss, CancellationToken::uncancelable()};
    synchronizer.start(operationContext(), getExecutor());

    // The delta splits the recovered chunk [100, 200) into [100, 150) and [150, 200). It is
    // enqueued before the disk pass produces the base, so it must be replayed on top of the
    // disk-recovered routing table during the drain.
    auto nextVersion = [v = chunks.back().getVersion()]() mutable {
        v.incMajor();
        return v;
    };
    const auto firstHalfVersion = nextVersion();
    const auto secondHalfVersion = nextVersion();
    synchronizer.onOplogEntry(
        lastWrittenTimestamp() + 1,
        makeDeltaEntry(splitChunk(chunks[1], 150, firstHalfVersion, secondHalfVersion)));

    ASSERT_OK(synchronizer.getMetadataFuture().getNoThrow(operationContext()));
    auto collMetadata = synchronizer.drainAndApply(operationContext());

    ASSERT_TRUE(collMetadata);
    ASSERT_EQ(collMetadata->getCollPlacementVersion(), secondHalfVersion);

    // The two new sub-ranges replaced the original [100, 200) chunk.
    assertChunkAt(
        *collMetadata, shardKey(120), shardKey(100), shardKey(150), kShard, firstHalfVersion);
    assertChunkAt(
        *collMetadata, shardKey(170), shardKey(150), shardKey(200), kShard, secondHalfVersion);
}

// Several queued deltas are applied, each introducing brand new chunks.
TEST_F(MetadataSynchronizerFixture, MetadataSynchronizerDrainsMultipleDeltas) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionMetadataSynchronizer synchronizer{kTestNss, CancellationToken::uncancelable()};
    synchronizer.start(operationContext(), getExecutor());

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
    synchronizer.onOplogEntry(baseTs + 1, makeDeltaEntry(frontSplit));
    synchronizer.onOplogEntry(baseTs + 2, makeDeltaEntry(secondSplit));

    ASSERT_OK(synchronizer.getMetadataFuture().getNoThrow(operationContext()));
    auto collMetadata = synchronizer.drainAndApply(operationContext());

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
// An invalidate queued after a delta forces the caller to discard this synchronizer; a new
// instance re-reads disk and is unaffected by the discarded delta.
TEST_F(MetadataSynchronizerFixture, MetadataSynchronizerRestartsWhenDeltaFollowedByInvalidate) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionMetadataSynchronizer synchronizer{kTestNss, CancellationToken::uncancelable()};
    synchronizer.start(operationContext(), getExecutor());
    ASSERT_OK(synchronizer.getMetadataFuture().getNoThrow(operationContext()));

    // A delta carrying new chunks (a split of [MinKey, 100) into [MinKey, 50) and [50, 100))
    // followed by an invalidate: the drain applies the delta, then the invalidate aborts this
    // synchronizer.
    auto lowVersion = chunks.back().getVersion();
    lowVersion.incMajor();
    auto highVersion = lowVersion;
    highVersion.incMajor();
    auto frontSplit = splitChunk(chunks.front(), 50, lowVersion, highVersion);
    const auto baseTs = lastWrittenTimestamp();
    synchronizer.onOplogEntry(baseTs + 1, makeDeltaEntry(frontSplit));
    synchronizer.onOplogEntry(baseTs + 2,
                              InvalidateCollectionMetadataOplogEntry{std::string(kTestNss.coll())});

    ASSERT_FALSE(synchronizer.drainAndApply(operationContext()));

    // New instance for the next round — same pattern as production recovery loop.
    CollectionMetadataSynchronizer nextSynchronizer{kTestNss, CancellationToken::uncancelable()};
    nextSynchronizer.start(operationContext(), getExecutor());
    ASSERT_OK(nextSynchronizer.getMetadataFuture().getNoThrow(operationContext()));
    auto collMetadata = nextSynchronizer.drainAndApply(operationContext());

    ASSERT_TRUE(collMetadata);
    ASSERT_EQ(collMetadata->getCollPlacementVersion(), chunks.back().getVersion());
}

// A delta older than the recovered version is ignored and leaves the metadata unchanged.
TEST_F(MetadataSynchronizerFixture, MetadataSynchronizerSkipsStaleDelta) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionMetadataSynchronizer synchronizer{kTestNss, CancellationToken::uncancelable()};
    synchronizer.start(operationContext(), getExecutor());

    // A delta whose version is below the recovered placement version is already reflected in the
    // recovered base, so it is idempotently ignored and leaves the routing table untouched.
    synchronizer.onOplogEntry(lastWrittenTimestamp() + 1, makeDeltaEntry({chunks.front()}));

    ASSERT_OK(synchronizer.getMetadataFuture().getNoThrow(operationContext()));
    auto collMetadata = synchronizer.drainAndApply(operationContext());

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
TEST_F(MetadataSynchronizerFixture, MetadataSynchronizerDropsDeltaBeforeRecoveryTimestamp) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionMetadataSynchronizer synchronizer{kTestNss, CancellationToken::uncancelable()};
    synchronizer.start(operationContext(), getExecutor());

    // An entry whose timestamp predates the recovery snapshot is already captured by the disk read,
    // so it must be dropped rather than enqueued.
    auto deltaVersion = chunks.back().getVersion();
    deltaVersion.incMajor();
    auto bumpedChunk = chunks.front();
    bumpedChunk.setVersion(deltaVersion);
    synchronizer.onOplogEntry(Timestamp(1, 0), makeDeltaEntry({bumpedChunk}));

    ASSERT_OK(synchronizer.getMetadataFuture().getNoThrow(operationContext()));
    auto collMetadata = synchronizer.drainAndApply(operationContext());

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

// Snapshot reads at T include writes committed at T, so an entry stamped with the recovery
// timestamp is already on disk and must not be replayed.
TEST_F(MetadataSynchronizerFixture, MetadataSynchronizerDropsEntriesAtRecoveryTimestamp) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionMetadataSynchronizer synchronizer{kTestNss, CancellationToken::uncancelable()};
    synchronizer.start(operationContext(), getExecutor());

    const auto recoveryTs = lastWrittenTimestamp();

    auto deltaVersion = chunks.back().getVersion();
    deltaVersion.incMajor();
    auto bumpedChunk = chunks.front();
    bumpedChunk.setVersion(deltaVersion);
    synchronizer.onOplogEntry(recoveryTs, makeDeltaEntry({bumpedChunk}));
    synchronizer.onOplogEntry(recoveryTs,
                              InvalidateCollectionMetadataOplogEntry{std::string(kTestNss.coll())});

    ASSERT_OK(synchronizer.getMetadataFuture().getNoThrow(operationContext()));
    auto collMetadata = synchronizer.drainAndApply(operationContext());

    // Both boundary entries were dropped: drain succeeds (invalidate not applied) and the disk
    // placement version is unchanged (delta not applied).
    ASSERT_TRUE(collMetadata);
    ASSERT_EQ(collMetadata->getCollPlacementVersion(), chunks.back().getVersion());
    assertChunkAt(*collMetadata,
                  shardKey(50),
                  chunks.front().getMin(),
                  chunks.front().getMax(),
                  kShard,
                  chunks.front().getVersion());
}

TEST_F(MetadataSynchronizerFixture,
       MetadataSynchronizerCanRecoverFromDiskWithConcurrentOplogEntries) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    auto collMetadata = [&]() {
        CollectionMetadataSynchronizer synchronizer{kTestNss, CancellationToken::uncancelable()};
        synchronizer.start(operationContext(), getExecutor());
        ASSERT_OK(synchronizer.getMetadataFuture().getNoThrow(operationContext()));
        auto recoveryTimestamp =
            repl::ReplicationCoordinator::get(operationContext())->getMyLastWrittenOpTime();
        synchronizer.onOplogEntry(
            recoveryTimestamp.getTimestamp() + 1,
            InvalidateCollectionMetadataOplogEntry{std::string(kTestNss.coll())});
        // Invalidate aborts this synchronizer; caller must construct a new one.
        ASSERT_FALSE(synchronizer.drainAndApply(operationContext()));

        CollectionMetadataSynchronizer nextSynchronizer{kTestNss,
                                                        CancellationToken::uncancelable()};
        nextSynchronizer.start(operationContext(), getExecutor());
        ASSERT_OK(nextSynchronizer.getMetadataFuture().getNoThrow(operationContext()));
        auto result = nextSynchronizer.drainAndApply(operationContext());
        ASSERT_TRUE(result);
        return result;
    }();

    const auto shardVersionExpected = chunks.back().getVersion();

    ASSERT_EQ(collMetadata->getCollPlacementVersion(), shardVersionExpected);
}

TEST_F(MetadataSynchronizerFixture, MetadataSynchronizerBubblesUpCachePressureErrors) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    FailPointEnableBlock intermittentFailure{"WTWriteConflictExceptionForReads"};

    CollectionMetadataSynchronizer synchronizer{kTestNss, CancellationToken::uncancelable()};

    synchronizer.start(operationContext(), getExecutor());
    auto status = synchronizer.getMetadataFuture().getNoThrow(operationContext());
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.getStatus().code(), ErrorCodes::WriteConflict);
}

TEST_F(MetadataSynchronizerFixture, MetadataSynchronizerBubblesUpDiskReadingFailure) {
    OperationContext* opCtx = operationContext();

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    {
        DBDirectClient client(opCtx);
        client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace,
                      BSON("_id" << kTestNss.toStringForErrorMsg() << "made_up" << true));
    }

    {
        CollectionMetadataSynchronizer synchronizer{kTestNss, CancellationToken::uncancelable()};

        // The CollectionType is parsed via an IDL parser. So it should throw an IDL failure.
        synchronizer.start(operationContext(), getExecutor());
        auto status = synchronizer.getMetadataFuture().getNoThrow(operationContext());
        ASSERT_NOT_OK(status);
        ASSERT_EQ(status.getStatus().code(), ErrorCodes::IDLFailedToParse);
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
        CollectionMetadataSynchronizer synchronizer{kTestNss, CancellationToken::uncancelable()};

        // The ChunkType uses a custom parser that returns a different family of errors compared to
        // the CollectionType. Let's make sure that's the case.
        synchronizer.start(operationContext(), getExecutor());
        auto status = synchronizer.getMetadataFuture().getNoThrow(operationContext());
        ASSERT_NOT_OK(status);
        ASSERT_EQ(status.getStatus().code(), ErrorCodes::NoSuchKey);
    }
}

TEST_F(MetadataSynchronizerFixture, OplogEntriesBeforeRecoveryTimestampAreIgnored) {
    OperationContext* opCtx = operationContext();
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, 1, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionMetadataSynchronizer synchronizer{kTestNss, CancellationToken::uncancelable()};
    synchronizer.onOplogEntry(Timestamp(1, 0),
                              InvalidateCollectionMetadataOplogEntry{std::string(kTestNss.coll())});

    synchronizer.start(opCtx, getExecutor());
    ASSERT_OK(synchronizer.getMetadataFuture().getNoThrow(opCtx));
    auto collMetadata = synchronizer.drainAndApply(opCtx);
    ASSERT_TRUE(collMetadata);
    ASSERT_TRUE(collMetadata->isSharded());
    ASSERT_EQ(collMetadata->getCollPlacementVersion(), chunks.back().getVersion());
}

TEST_F(MetadataSynchronizerFixture, MetadataSynchronizerDrainFailsAfterInvalidate) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, kShard);
    seedShardCatalogOnDisk(opCtx, collType, chunks);

    CollectionMetadataSynchronizer synchronizer{kTestNss, CancellationToken::uncancelable()};

    synchronizer.start(operationContext(), getExecutor());
    ASSERT_OK(synchronizer.getMetadataFuture().getNoThrow(operationContext()));
    auto recoveryTimestamp =
        repl::ReplicationCoordinator::get(operationContext())->getMyLastWrittenOpTime();
    synchronizer.onOplogEntry(recoveryTimestamp.getTimestamp() + 1,
                              InvalidateCollectionMetadataOplogEntry{std::string(kTestNss.coll())});
    ASSERT_FALSE(synchronizer.drainAndApply(operationContext()));
}

}  // namespace mongo
