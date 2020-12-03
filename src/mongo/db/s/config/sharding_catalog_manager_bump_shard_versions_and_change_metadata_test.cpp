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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache_noop.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

const NamespaceString kNss("TestDB", "TestColl");
const KeyPattern kKeyPattern(BSON("a" << 1));
const ShardType kShard0("shard0000", "shard0000:1234");
const ShardType kShard1("shard0001", "shard0001:1234");

class ShardingCatalogManagerBumpShardVersionsAndChangeMetadataTest
    : public ConfigServerTestFixture {
    void setUp() {
        ConfigServerTestFixture::setUp();
        setupShards({kShard0, kShard1});

        // Create config.transactions collection.
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace.ns());
        client.createCollection(CollectionType::ConfigNS.ns());

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->onShardingInitialization(operationContext(), true);
    }

    void tearDown() {
        TransactionCoordinatorService::get(operationContext())->onStepDown();
        ConfigServerTestFixture::tearDown();
    }

protected:
    ChunkType generateChunkType(const NamespaceString& nss,
                                const ChunkVersion& chunkVersion,
                                const ShardId& shardId,
                                const BSONObj& minKey,
                                const BSONObj& maxKey) {
        ChunkType chunkType;
        chunkType.setName(OID::gen());
        chunkType.setNS(nss);
        chunkType.setVersion(chunkVersion);
        chunkType.setShard(shardId);
        chunkType.setMin(minKey);
        chunkType.setMax(maxKey);
        chunkType.setHistory({ChunkHistory(Timestamp(100, 0), shardId)});
        return chunkType;
    }

    /**
     * Determines if the chunk's version has been bumped to the targetChunkVersion.
     */
    bool chunkMajorVersionWasBumpedAndOtherFieldsAreUnchanged(
        const ChunkType& chunkTypeBefore,
        const StatusWith<ChunkType> swChunkTypeAfter,
        const ChunkVersion& targetChunkVersion) {
        ASSERT_OK(swChunkTypeAfter.getStatus());
        auto chunkTypeAfter = swChunkTypeAfter.getValue();

        // Regardless of whether the major version was bumped, the chunk's other fields should be
        // unchanged.
        ASSERT_EQ(chunkTypeBefore.getName(), chunkTypeAfter.getName());
        ASSERT_EQ(chunkTypeBefore.getNS(), chunkTypeAfter.getNS());
        ASSERT_BSONOBJ_EQ(chunkTypeBefore.getMin(), chunkTypeAfter.getMin());
        ASSERT_BSONOBJ_EQ(chunkTypeBefore.getMax(), chunkTypeAfter.getMax());
        ASSERT(chunkTypeBefore.getHistory() == chunkTypeAfter.getHistory());

        return chunkTypeAfter.getVersion().majorVersion() == targetChunkVersion.majorVersion();
    }

    /**
     * If there are multiple chunks per shard, the chunk whose version gets bumped is not
     * deterministic.
     *
     * Asserts that only chunk per shard has its major version increased.
     */
    void assertOnlyOneChunkVersionBumped(OperationContext* opCtx,
                                         std::vector<ChunkType> originalChunkTypes,
                                         const ChunkVersion& targetChunkVersion) {
        auto aChunkVersionWasBumped = false;
        for (auto originalChunkType : originalChunkTypes) {
            auto swChunkTypeAfter = getChunkDoc(opCtx, originalChunkType.getMin());
            auto wasBumped = chunkMajorVersionWasBumpedAndOtherFieldsAreUnchanged(
                originalChunkType, swChunkTypeAfter, targetChunkVersion);
            if (aChunkVersionWasBumped) {
                ASSERT_FALSE(wasBumped);
            } else {
                aChunkVersionWasBumped = wasBumped;
            }
        }

        ASSERT_TRUE(aChunkVersionWasBumped);
    }
};

TEST_F(ShardingCatalogManagerBumpShardVersionsAndChangeMetadataTest,
       BumpChunkVersionOneChunkPerShard) {
    const auto epoch = OID::gen();
    const auto shard0Chunk0 =
        generateChunkType(kNss,
                          ChunkVersion(10, 1, epoch, boost::none /* timestamp */),
                          kShard0.getName(),
                          BSON("a" << 1),
                          BSON("a" << 10));
    const auto shard1Chunk0 =
        generateChunkType(kNss,
                          ChunkVersion(11, 2, epoch, boost::none /* timestamp */),
                          kShard1.getName(),
                          BSON("a" << 11),
                          BSON("a" << 20));

    const auto collectionVersion = shard1Chunk0.getVersion();
    ChunkVersion targetChunkVersion(collectionVersion.majorVersion() + 1,
                                    0,
                                    collectionVersion.epoch(),
                                    collectionVersion.getTimestamp());

    setupCollection(kNss, kKeyPattern, {shard0Chunk0, shard1Chunk0});

    auto opCtx = operationContext();

    std::vector<ShardId> shardIds{kShard0.getName(), kShard1.getName()};
    ShardingCatalogManager::get(opCtx)->bumpCollShardVersionsAndChangeMetadataInTxn(
        opCtx, kNss, shardIds, [&](OperationContext*, TxnNumber) {});

    ASSERT_TRUE(chunkMajorVersionWasBumpedAndOtherFieldsAreUnchanged(
        shard0Chunk0, getChunkDoc(operationContext(), shard0Chunk0.getMin()), targetChunkVersion));

    ASSERT_TRUE(chunkMajorVersionWasBumpedAndOtherFieldsAreUnchanged(
        shard1Chunk0, getChunkDoc(operationContext(), shard1Chunk0.getMin()), targetChunkVersion));
}

TEST_F(ShardingCatalogManagerBumpShardVersionsAndChangeMetadataTest,
       BumpChunkVersionTwoChunksOnOneShard) {
    const auto epoch = OID::gen();
    const auto shard0Chunk0 =
        generateChunkType(kNss,
                          ChunkVersion(10, 1, epoch, boost::none /* timestamp */),
                          kShard0.getName(),
                          BSON("a" << 1),
                          BSON("a" << 10));
    const auto shard0Chunk1 =
        generateChunkType(kNss,
                          ChunkVersion(11, 2, epoch, boost::none /* timestamp */),
                          kShard0.getName(),
                          BSON("a" << 11),
                          BSON("a" << 20));
    const auto shard1Chunk0 =
        generateChunkType(kNss,
                          ChunkVersion(8, 1, epoch, boost::none /* timestamp */),
                          kShard1.getName(),
                          BSON("a" << 21),
                          BSON("a" << 100));

    const auto collectionVersion = shard0Chunk1.getVersion();
    ChunkVersion targetChunkVersion(collectionVersion.majorVersion() + 1,
                                    0,
                                    collectionVersion.epoch(),
                                    collectionVersion.getTimestamp());

    setupCollection(kNss, kKeyPattern, {shard0Chunk0, shard0Chunk1, shard1Chunk0});

    auto opCtx = operationContext();
    std::vector<ShardId> shardIds{kShard0.getName(), kShard1.getName()};
    ShardingCatalogManager::get(opCtx)->bumpCollShardVersionsAndChangeMetadataInTxn(
        opCtx, kNss, shardIds, [&](OperationContext*, TxnNumber) {});

    assertOnlyOneChunkVersionBumped(
        operationContext(), {shard0Chunk0, shard0Chunk1}, targetChunkVersion);

    ASSERT_TRUE(chunkMajorVersionWasBumpedAndOtherFieldsAreUnchanged(
        shard1Chunk0, getChunkDoc(operationContext(), shard1Chunk0.getMin()), targetChunkVersion));
}

TEST_F(ShardingCatalogManagerBumpShardVersionsAndChangeMetadataTest,
       BumpChunkVersionTwoChunksOnTwoShards) {
    const auto epoch = OID::gen();
    const auto shard0Chunk0 =
        generateChunkType(kNss,
                          ChunkVersion(10, 1, epoch, boost::none /* timestamp */),
                          kShard0.getName(),
                          BSON("a" << 1),
                          BSON("a" << 10));
    const auto shard0Chunk1 =
        generateChunkType(kNss,
                          ChunkVersion(11, 2, epoch, boost::none /* timestamp */),
                          kShard0.getName(),
                          BSON("a" << 11),
                          BSON("a" << 20));
    const auto shard1Chunk0 =
        generateChunkType(kNss,
                          ChunkVersion(8, 1, epoch, boost::none /* timestamp */),
                          kShard1.getName(),
                          BSON("a" << 21),
                          BSON("a" << 100));
    const auto shard1Chunk1 =
        generateChunkType(kNss,
                          ChunkVersion(12, 1, epoch, boost::none /* timestamp */),
                          kShard1.getName(),
                          BSON("a" << 101),
                          BSON("a" << 200));

    const auto collectionVersion = shard1Chunk1.getVersion();
    ChunkVersion targetChunkVersion(collectionVersion.majorVersion() + 1,
                                    0,
                                    collectionVersion.epoch(),
                                    collectionVersion.getTimestamp());

    setupCollection(kNss, kKeyPattern, {shard0Chunk0, shard0Chunk1, shard1Chunk0, shard1Chunk1});

    auto opCtx = operationContext();
    std::vector<ShardId> shardIds{kShard0.getName(), kShard1.getName()};
    ShardingCatalogManager::get(opCtx)->bumpCollShardVersionsAndChangeMetadataInTxn(
        opCtx, kNss, shardIds, [&](OperationContext*, TxnNumber) {});

    assertOnlyOneChunkVersionBumped(
        operationContext(), {shard0Chunk0, shard0Chunk1}, targetChunkVersion);

    assertOnlyOneChunkVersionBumped(
        operationContext(), {shard1Chunk0, shard1Chunk1}, targetChunkVersion);
}

TEST_F(ShardingCatalogManagerBumpShardVersionsAndChangeMetadataTest,
       SucceedsInThePresenceOfTransientTransactionErrors) {
    const auto epoch = OID::gen();
    const auto shard0Chunk0 =
        generateChunkType(kNss,
                          ChunkVersion(10, 1, epoch, boost::none /* timestamp */),
                          kShard0.getName(),
                          BSON("a" << 1),
                          BSON("a" << 10));
    const auto shard1Chunk0 =
        generateChunkType(kNss,
                          ChunkVersion(11, 2, epoch, boost::none /* timestamp */),
                          kShard1.getName(),
                          BSON("a" << 11),
                          BSON("a" << 20));
    const auto initialCollectionVersion = shard1Chunk0.getVersion();

    setupCollection(kNss, kKeyPattern, {shard0Chunk0, shard1Chunk0});

    size_t numCalls = 0;
    const std::vector<ShardId> shardIds{kShard0.getName(), kShard1.getName()};
    ShardingCatalogManager::get(operationContext())
        ->bumpCollShardVersionsAndChangeMetadataInTxn(
            operationContext(), kNss, shardIds, [&](OperationContext*, TxnNumber) {
                ++numCalls;
                if (numCalls < 5) {
                    throw WriteConflictException();
                }
            });

    auto targetChunkVersion = ChunkVersion{initialCollectionVersion.majorVersion() + 1,
                                           0,
                                           initialCollectionVersion.epoch(),
                                           initialCollectionVersion.getTimestamp()};

    ASSERT_TRUE(chunkMajorVersionWasBumpedAndOtherFieldsAreUnchanged(
        shard0Chunk0, getChunkDoc(operationContext(), shard0Chunk0.getMin()), targetChunkVersion));

    ASSERT_TRUE(chunkMajorVersionWasBumpedAndOtherFieldsAreUnchanged(
        shard1Chunk0, getChunkDoc(operationContext(), shard1Chunk0.getMin()), targetChunkVersion));

    ASSERT_EQ(numCalls, 5) << "transaction succeeded after unexpected number of attempts";

    auto fp = std::make_unique<FailPointEnableBlock>(
        "failCommand",
        BSON("errorCode" << ErrorCodes::LockTimeout << "failCommands"
                         << BSON_ARRAY("commitTransaction") << "failLocalClients" << true
                         << "failInternalCommands" << true));

    numCalls = 0;
    ShardingCatalogManager::get(operationContext())
        ->bumpCollShardVersionsAndChangeMetadataInTxn(
            operationContext(), kNss, shardIds, [&](OperationContext*, TxnNumber) {
                ++numCalls;
                if (numCalls >= 5) {
                    fp.reset();
                }
            });

    targetChunkVersion = ChunkVersion{initialCollectionVersion.majorVersion() + 2,
                                      0,
                                      initialCollectionVersion.epoch(),
                                      initialCollectionVersion.getTimestamp()};

    ASSERT_TRUE(chunkMajorVersionWasBumpedAndOtherFieldsAreUnchanged(
        shard0Chunk0, getChunkDoc(operationContext(), shard0Chunk0.getMin()), targetChunkVersion));

    ASSERT_TRUE(chunkMajorVersionWasBumpedAndOtherFieldsAreUnchanged(
        shard1Chunk0, getChunkDoc(operationContext(), shard1Chunk0.getMin()), targetChunkVersion));

    ASSERT_EQ(numCalls, 5) << "transaction succeeded after unexpected number of attempts";
}

TEST_F(ShardingCatalogManagerBumpShardVersionsAndChangeMetadataTest,
       StopsRetryingOnPermanentServerErrors) {
    const auto epoch = OID::gen();
    const auto shard0Chunk0 =
        generateChunkType(kNss,
                          ChunkVersion(10, 1, epoch, boost::none /* timestamp */),
                          kShard0.getName(),
                          BSON("a" << 1),
                          BSON("a" << 10));
    const auto shard1Chunk0 =
        generateChunkType(kNss,
                          ChunkVersion(11, 2, epoch, boost::none /* timestamp */),
                          kShard1.getName(),
                          BSON("a" << 11),
                          BSON("a" << 20));

    setupCollection(kNss, kKeyPattern, {shard0Chunk0, shard1Chunk0});

    size_t numCalls = 0;
    const std::vector<ShardId> shardIds{kShard0.getName(), kShard1.getName()};
    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->bumpCollShardVersionsAndChangeMetadataInTxn(
                               operationContext(),
                               kNss,
                               shardIds,
                               [&](OperationContext*, TxnNumber) {
                                   ++numCalls;
                                   uasserted(ErrorCodes::ShutdownInProgress,
                                             "simulating shutdown error from test");
                               }),
                       DBException,
                       ErrorCodes::ShutdownInProgress);

    ASSERT_TRUE(chunkMajorVersionWasBumpedAndOtherFieldsAreUnchanged(
        shard0Chunk0,
        getChunkDoc(operationContext(), shard0Chunk0.getMin()),
        shard0Chunk0.getVersion()));

    ASSERT_TRUE(chunkMajorVersionWasBumpedAndOtherFieldsAreUnchanged(
        shard1Chunk0,
        getChunkDoc(operationContext(), shard1Chunk0.getMin()),
        shard1Chunk0.getVersion()));

    ASSERT_EQ(numCalls, 1) << "transaction failed after unexpected number of attempts";

    numCalls = 0;
    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->bumpCollShardVersionsAndChangeMetadataInTxn(
                               operationContext(),
                               kNss,
                               shardIds,
                               [&](OperationContext*, TxnNumber) {
                                   ++numCalls;
                                   uasserted(ErrorCodes::NotWritablePrimary,
                                             "simulating not writable primary error from test");
                               }),
                       DBException,
                       ErrorCodes::NotWritablePrimary);

    ASSERT_TRUE(chunkMajorVersionWasBumpedAndOtherFieldsAreUnchanged(
        shard0Chunk0,
        getChunkDoc(operationContext(), shard0Chunk0.getMin()),
        shard0Chunk0.getVersion()));

    ASSERT_TRUE(chunkMajorVersionWasBumpedAndOtherFieldsAreUnchanged(
        shard1Chunk0,
        getChunkDoc(operationContext(), shard1Chunk0.getMin()),
        shard1Chunk0.getVersion()));

    ASSERT_EQ(numCalls, 1) << "transaction failed after unexpected number of attempts";

    numCalls = 0;
    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->bumpCollShardVersionsAndChangeMetadataInTxn(
                               operationContext(),
                               kNss,
                               shardIds,
                               [&](OperationContext*, TxnNumber) {
                                   ++numCalls;
                                   cc().getOperationContext()->markKilled(ErrorCodes::Interrupted);

                                   // Throw a LockTimeout exception so
                                   // bumpCollShardVersionsAndChangeMetadataInTxn() makes another
                                   // retry attempt and discovers operation context has been killed.
                                   uasserted(ErrorCodes::LockTimeout,
                                             "simulating lock timeout error from test");
                               }),
                       DBException,
                       ErrorCodes::Interrupted);

    ASSERT_TRUE(chunkMajorVersionWasBumpedAndOtherFieldsAreUnchanged(
        shard0Chunk0,
        getChunkDoc(operationContext(), shard0Chunk0.getMin()),
        shard0Chunk0.getVersion()));

    ASSERT_TRUE(chunkMajorVersionWasBumpedAndOtherFieldsAreUnchanged(
        shard1Chunk0,
        getChunkDoc(operationContext(), shard1Chunk0.getMin()),
        shard1Chunk0.getVersion()));

    ASSERT_EQ(numCalls, 1) << "transaction failed after unexpected number of attempts";
}

}  // namespace
}  // namespace mongo
