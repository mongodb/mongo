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

#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

const NamespaceString kNss("TestDB", "TestColl");
const KeyPattern kKeyPattern(BSON("a" << 1));
const ShardType kShard0("shard0000", "shard0000:1234");
const ShardType kShard1("shard0001", "shard0001:1234");

class ShardingCatalogManagerBumpCollectionVersionAndChangeMetadataTest
    : public ConfigServerTestFixture {
    void setUp() {
        ConfigServerTestFixture::setUp();
        setupShards({kShard0, kShard1});

        // Create config.transactions collection.
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace.ns());
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});
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
    ChunkType generateChunkType(const UUID& uuid,
                                const ChunkVersion& chunkVersion,
                                const ShardId& shardId,
                                const BSONObj& minKey,
                                const BSONObj& maxKey) {
        ChunkType chunkType;
        chunkType.setName(OID::gen());
        chunkType.setCollectionUUID(uuid);
        chunkType.setVersion(chunkVersion);
        chunkType.setShard(shardId);
        chunkType.setMin(minKey);
        chunkType.setMax(maxKey);
        chunkType.setOnCurrentShardSince(Timestamp(100, 0));
        chunkType.setHistory({ChunkHistory(*chunkType.getOnCurrentShardSince(), shardId)});
        return chunkType;
    }

    void assertChunkUnchanged(const ChunkType& chunkTypeBefore) {
        const auto chunkTypeNow =
            uassertStatusOK(getChunkDoc(operationContext(),
                                        chunkTypeBefore.getMin(),
                                        chunkTypeBefore.getVersion().epoch(),
                                        chunkTypeBefore.getVersion().getTimestamp()));
        ASSERT_BSONOBJ_EQ(chunkTypeBefore.toConfigBSON(), chunkTypeNow.toConfigBSON());
    }

    void assertChunkVersionChangedAndOtherFieldsUnchanged(const ChunkType& chunkTypeBefore,
                                                          const ChunkVersion& targetChunkVersion) {
        const auto chunkTypeNow = uassertStatusOK(getChunkDoc(operationContext(),
                                                              chunkTypeBefore.getMin(),
                                                              targetChunkVersion.epoch(),
                                                              targetChunkVersion.getTimestamp()));
        ASSERT_BSONOBJ_EQ(chunkTypeBefore.toConfigBSON().removeField(ChunkType::lastmod()),
                          chunkTypeNow.toConfigBSON().removeField(ChunkType::lastmod()));
        ASSERT_EQ(targetChunkVersion, chunkTypeNow.getVersion());
    }
};

TEST_F(ShardingCatalogManagerBumpCollectionVersionAndChangeMetadataTest,
       BumpsOnlyMinorVersionOfNewestChunk) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);
    const auto collUUID = UUID::gen();

    const auto shard0Chunk0 = generateChunkType(collUUID,
                                                ChunkVersion({collEpoch, collTimestamp}, {10, 1}),
                                                kShard0.getName(),
                                                BSON("a" << 1),
                                                BSON("a" << 10));
    const auto shard0Chunk1 = generateChunkType(collUUID,
                                                ChunkVersion({collEpoch, collTimestamp}, {11, 2}),
                                                kShard0.getName(),
                                                BSON("a" << 11),
                                                BSON("a" << 20));
    const auto shard1Chunk0 = generateChunkType(collUUID,
                                                ChunkVersion({collEpoch, collTimestamp}, {8, 1}),
                                                kShard1.getName(),
                                                BSON("a" << 21),
                                                BSON("a" << 100));

    const auto collectionVersion = shard0Chunk1.getVersion();
    auto targetCollectionVersion = collectionVersion;
    targetCollectionVersion.incMinor();

    setupCollection(kNss, kKeyPattern, {shard0Chunk0, shard0Chunk1, shard1Chunk0});

    auto opCtx = operationContext();
    ShardingCatalogManager::get(opCtx)->bumpCollectionVersionAndChangeMetadataInTxn(
        opCtx, kNss, [&](OperationContext*, TxnNumber) {});

    assertChunkUnchanged(shard0Chunk0);
    assertChunkVersionChangedAndOtherFieldsUnchanged(shard0Chunk1, targetCollectionVersion);
    assertChunkUnchanged(shard1Chunk0);
}

TEST_F(ShardingCatalogManagerBumpCollectionVersionAndChangeMetadataTest, NoChunks) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);
    const auto collUUID = UUID::gen();

    const auto shard0Chunk0 = generateChunkType(collUUID,
                                                ChunkVersion({collEpoch, collTimestamp}, {10, 1}),
                                                kShard0.getName(),
                                                BSON("a" << 1),
                                                BSON("a" << 10));

    setupCollection(kNss, kKeyPattern, {shard0Chunk0});

    auto opCtx = operationContext();
    ASSERT_OK(deleteToConfigCollection(
        opCtx, ChunkType::ConfigNS, BSON(ChunkType::name << shard0Chunk0.getName()), false));

    ASSERT_THROWS_CODE(
        ShardingCatalogManager::get(opCtx)->bumpCollectionVersionAndChangeMetadataInTxn(
            opCtx, kNss, [&](OperationContext*, TxnNumber) {}),
        DBException,
        ErrorCodes::IncompatibleShardingMetadata);
}

TEST_F(ShardingCatalogManagerBumpCollectionVersionAndChangeMetadataTest,
       SucceedsInThePresenceOfTransientTransactionErrors) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);
    const auto collUUID = UUID::gen();

    const auto shard0Chunk0 = generateChunkType(collUUID,
                                                ChunkVersion({collEpoch, collTimestamp}, {10, 1}),
                                                kShard0.getName(),
                                                BSON("a" << 1),
                                                BSON("a" << 10));
    const auto shard1Chunk0 = generateChunkType(collUUID,
                                                ChunkVersion({collEpoch, collTimestamp}, {11, 2}),
                                                kShard1.getName(),
                                                BSON("a" << 11),
                                                BSON("a" << 20));
    const auto initialCollectionVersion = shard1Chunk0.getVersion();

    setupCollection(kNss, kKeyPattern, {shard0Chunk0, shard1Chunk0});

    size_t numCalls = 0;
    ShardingCatalogManager::get(operationContext())
        ->bumpCollectionVersionAndChangeMetadataInTxn(
            operationContext(), kNss, [&](OperationContext*, TxnNumber) {
                ++numCalls;
                if (numCalls < 5) {
                    throwWriteConflictException("Simulating transient transaction errors.");
                }
            });

    auto targetCollectionVersion = initialCollectionVersion;
    targetCollectionVersion.incMinor();

    assertChunkUnchanged(shard0Chunk0);
    assertChunkVersionChangedAndOtherFieldsUnchanged(shard1Chunk0, targetCollectionVersion);

    ASSERT_EQ(numCalls, 5) << "transaction succeeded after unexpected number of attempts";

    auto fp = std::make_unique<FailPointEnableBlock>(
        "failCommand",
        BSON("errorCode" << ErrorCodes::LockTimeout << "failCommands"
                         << BSON_ARRAY("commitTransaction") << "failLocalClients" << true
                         << "failInternalCommands" << true));

    numCalls = 0;
    ShardingCatalogManager::get(operationContext())
        ->bumpCollectionVersionAndChangeMetadataInTxn(
            operationContext(), kNss, [&](OperationContext*, TxnNumber) {
                ++numCalls;
                if (numCalls >= 5) {
                    fp.reset();
                }
            });

    targetCollectionVersion.incMinor();

    assertChunkUnchanged(shard0Chunk0);
    assertChunkVersionChangedAndOtherFieldsUnchanged(shard1Chunk0, targetCollectionVersion);

    ASSERT_EQ(numCalls, 5) << "transaction succeeded after unexpected number of attempts";
}

TEST_F(ShardingCatalogManagerBumpCollectionVersionAndChangeMetadataTest,
       StopsRetryingOnPermanentServerErrors) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);
    const auto collUUID = UUID::gen();

    const auto shard0Chunk0 = generateChunkType(collUUID,
                                                ChunkVersion({collEpoch, collTimestamp}, {10, 1}),
                                                kShard0.getName(),
                                                BSON("a" << 1),
                                                BSON("a" << 10));
    const auto shard1Chunk0 = generateChunkType(collUUID,
                                                ChunkVersion({collEpoch, collTimestamp}, {11, 2}),
                                                kShard1.getName(),
                                                BSON("a" << 11),
                                                BSON("a" << 20));

    setupCollection(kNss, kKeyPattern, {shard0Chunk0, shard1Chunk0});

    size_t numCalls = 0;
    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->bumpCollectionVersionAndChangeMetadataInTxn(
                               operationContext(),
                               kNss,
                               [&](OperationContext*, TxnNumber) {
                                   ++numCalls;
                                   uasserted(ErrorCodes::ShutdownInProgress,
                                             "simulating shutdown error from test");
                               }),
                       DBException,
                       ErrorCodes::ShutdownInProgress);

    assertChunkUnchanged(shard0Chunk0);
    assertChunkUnchanged(shard1Chunk0);

    ASSERT_EQ(numCalls, 1) << "transaction failed after unexpected number of attempts";

    numCalls = 0;
    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->bumpCollectionVersionAndChangeMetadataInTxn(
                               operationContext(),
                               kNss,
                               [&](OperationContext*, TxnNumber) {
                                   ++numCalls;
                                   uasserted(ErrorCodes::NotWritablePrimary,
                                             "simulating not writable primary error from test");
                               }),
                       DBException,
                       ErrorCodes::NotWritablePrimary);

    assertChunkUnchanged(shard0Chunk0);
    assertChunkUnchanged(shard1Chunk0);

    ASSERT_EQ(numCalls, 1) << "transaction failed after unexpected number of attempts";

    numCalls = 0;
    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->bumpCollectionVersionAndChangeMetadataInTxn(
                               operationContext(),
                               kNss,
                               [&](OperationContext*, TxnNumber) {
                                   ++numCalls;
                                   cc().getOperationContext()->markKilled(ErrorCodes::Interrupted);

                                   // Throw a LockTimeout exception so
                                   // bumpCollectionVersionAndChangeMetadataInTxn() makes another
                                   // retry attempt and discovers operation context has been killed.
                                   uasserted(ErrorCodes::LockTimeout,
                                             "simulating lock timeout error from test");
                               }),
                       DBException,
                       ErrorCodes::Interrupted);

    assertChunkUnchanged(shard0Chunk0);
    assertChunkUnchanged(shard1Chunk0);

    /*ASSERT_TRUE(chunkMajorVersionWasBumpedAndOtherFieldsAreUnchanged(
        shard0Chunk0,
        getChunkDoc(operationContext(), shard0Chunk0.getMin(), collEpoch, collTimestamp),
        shard0Chunk0.getVersion()));

    ASSERT_TRUE(chunkMajorVersionWasBumpedAndOtherFieldsAreUnchanged(
        shard1Chunk0,
        getChunkDoc(operationContext(), shard1Chunk0.getMin(), collEpoch, collTimestamp),
        shard1Chunk0.getVersion()));*/

    ASSERT_EQ(numCalls, 1) << "transaction failed after unexpected number of attempts";
}

}  // namespace
}  // namespace mongo
