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
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/unittest/unittest.h"

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

class RecovererFixture : public ShardServerTestFixture {
protected:
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
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, ShardId("0"));

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    {
        DBDirectClient client(opCtx);
        client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace, collType.toBSON());
    }

    for (const auto& chunk : chunks) {
        DBDirectClient client(opCtx);
        client.insert(NamespaceString::kConfigShardCatalogChunksNamespace, chunk.toConfigBSON());
    }

    CollectionCacheRecoverer recoverer{kTestNss};

    auto roundId = recoverer.start(operationContext(), getExecutor());
    ASSERT_OK(recoverer.waitForInitialPass(operationContext(), roundId));
    auto collMetadata = recoverer.drainAndApply(operationContext(), roundId);

    ASSERT_TRUE(collMetadata);

    const auto shardVersionExpected = chunks.back().getVersion();

    ASSERT_EQ(collMetadata->getCollPlacementVersion(), shardVersionExpected);
}

TEST_F(RecovererFixture, CacheRecovererAppliesOplogChanges) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, ShardId("0"));

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    {
        DBDirectClient client(opCtx);
        client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace, collType.toBSON());
    }

    for (const auto& chunk : chunks) {
        DBDirectClient client(opCtx);
        client.insert(NamespaceString::kConfigShardCatalogChunksNamespace, chunk.toConfigBSON());
    }

    auto collMetadata = [&] {
        CollectionCacheRecoverer recoverer{kTestNss};

        auto roundId = recoverer.start(operationContext(), getExecutor());
        ASSERT_OK(recoverer.waitForInitialPass(operationContext(), roundId));
        return recoverer.drainAndApply(operationContext(), roundId);
    }();

    ASSERT_TRUE(collMetadata);

    CollectionCacheRecoverer recoverer{kTestNss, std::move(*collMetadata)};
    auto roundId = recoverer.start(operationContext(), getExecutor());
    recoverer.onOplogEntry(
        operationContext(), Timestamp(Date_t::now()), CollectionShardingStateDeltaOplogEntry{});
    collMetadata = recoverer.drainAndApply(operationContext(), roundId);

    ASSERT_TRUE(collMetadata);
}

TEST_F(RecovererFixture, CacheRecovererCanRecoverFromDiskWithConcurrentOplogEntries) {
    OperationContext* opCtx = operationContext();
    int numChunks = 20;
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, ShardId("0"));

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    {
        DBDirectClient client(opCtx);
        client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace, collType.toBSON());
    }

    for (const auto& chunk : chunks) {
        DBDirectClient client(opCtx);
        client.insert(NamespaceString::kConfigShardCatalogChunksNamespace, chunk.toConfigBSON());
    }

    CollectionCacheRecoverer recoverer{kTestNss};

    auto collMetadata = [&]() {
        auto roundId = recoverer.start(operationContext(), getExecutor());
        ASSERT_OK(recoverer.waitForInitialPass(operationContext(), roundId));
        auto recoveryTimestamp =
            repl::ReplicationCoordinator::get(operationContext())->getMyLastWrittenOpTime();
        // We now add an oplog entry that invalidates the previous recovery.
        recoverer.onOplogEntry(
            operationContext(),
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
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, ShardId("0"));

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    {
        DBDirectClient client(opCtx);
        client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace, collType.toBSON());
    }

    for (const auto& chunk : chunks) {
        DBDirectClient client(opCtx);
        client.insert(NamespaceString::kConfigShardCatalogChunksNamespace, chunk.toConfigBSON());
    }

    FailPointEnableBlock intermittentFailure{"WTWriteConflictExceptionForReads"};

    CollectionCacheRecoverer recoverer{kTestNss};

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
        CollectionCacheRecoverer recoverer{kTestNss};

        // The CollectionType is parsed via an IDL parser. So it should throw an IDL failure.
        auto roundId = recoverer.start(operationContext(), getExecutor());
        auto status = recoverer.waitForInitialPass(operationContext(), roundId);
        ASSERT_NOT_OK(status);
        ASSERT_EQ(status.code(), ErrorCodes::IDLFailedToParse);
    }

    int numChunks = 20;
    const auto [collType, _] = makeShardedMetadataForDisk(opCtx, numChunks, ShardId("0"));

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
        CollectionCacheRecoverer recoverer{kTestNss};

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
    const auto [collType, chunks] = makeShardedMetadataForDisk(opCtx, numChunks, ShardId("0"));

    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogCollectionsNamespace);
    createTestCollection(opCtx, NamespaceString::kConfigShardCatalogChunksNamespace);

    {
        DBDirectClient client(opCtx);
        client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace, collType.toBSON());
    }

    for (const auto& chunk : chunks) {
        DBDirectClient client(opCtx);
        client.insert(NamespaceString::kConfigShardCatalogChunksNamespace, chunk.toConfigBSON());
    }

    CollectionCacheRecoverer recoverer{kTestNss};

    auto roundId = recoverer.start(operationContext(), getExecutor());
    ASSERT_OK(recoverer.waitForInitialPass(operationContext(), roundId));
    // We now add an oplog entry that invalidates the previous recovery.
    auto recoveryTimestamp =
        repl::ReplicationCoordinator::get(operationContext())->getMyLastWrittenOpTime();
    recoverer.onOplogEntry(operationContext(),
                           recoveryTimestamp.getTimestamp() + 1,
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
