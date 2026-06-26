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

#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/sharding_catalog_client_mock.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/range_deleter_service_test.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/db/sharding_environment/shard_server_op_observer.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_mongod_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"

#include <barrier>
#include <chrono>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const std::string kShardKey = "_id";
const BSONObj kShardKeyPattern = BSON(kShardKey << 1);
const ShardHandle kThisShardHandle = ShardHandle(ShardId("thisShard"), UUID::gen());
const ShardHandle kOtherShardHandle = ShardHandle(ShardId("otherShard"), UUID::gen());

class CollectionShardingRuntimeTest : public ShardServerTestFixture {
public:
    static CollectionMetadata makeShardedMetadata(
        OperationContext* opCtx,
        UUID uuid = UUID::gen(),
        ShardHandle chunkShardHandle = kOtherShardHandle,
        ShardHandle collectionShardHandle = kThisShardHandle) {
        const OID epoch = OID::gen();
        const Timestamp timestamp(Date_t::now());

        // Sleeping some time here to guarantee that any upcoming call to this function generates a
        // different timestamp
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        auto range = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY));
        auto chunk = ChunkType(uuid,
                               std::move(range),
                               ChunkVersion({epoch, timestamp}, {1, 0}),
                               chunkShardHandle.toShardRef(opCtx));
        CurrentChunkManager cm(makeStandaloneRoutingTableHistory(
            RoutingTableHistory::makeNew(kTestNss,
                                         uuid,
                                         kShardKeyPattern,
                                         false, /* unsplittable */
                                         nullptr,
                                         false,
                                         epoch,
                                         timestamp,
                                         boost::none /* timeseriesFields */,
                                         boost::none /* reshardingFields */,

                                         true,
                                         {std::move(chunk)})));

        return CollectionMetadata(std::move(cm), collectionShardHandle);
    }

    static CollectionMetadata changeShardVersion(OperationContext* opCtx,
                                                 const CollectionMetadata& metadata,
                                                 const ChunkVersion& newVersion) {
        auto range = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY));
        auto chunk = ChunkType(metadata.getUUID(),
                               std::move(range),
                               newVersion,
                               metadata.shardHandle().toShardRef(opCtx));
        CurrentChunkManager cm(makeStandaloneRoutingTableHistory(
            RoutingTableHistory::makeNew(metadata.getChunkManager()->getNss(),
                                         metadata.getUUID(),
                                         kShardKeyPattern,
                                         false, /* unsplittable */
                                         nullptr,
                                         false,
                                         newVersion.epoch(),
                                         newVersion.getTimestamp(),
                                         boost::none /* timeseriesFields */,
                                         boost::none /* reshardingFields */,
                                         true,
                                         {std::move(chunk)})));
        return CollectionMetadata{cm, metadata.shardHandle()};
    }

    // Builds sharded metadata over a gap-allowing routing table containing only the given owned
    // chunks, mirroring how the shard catalog builds filtering metadata from the chunks a shard
    // actually owns. Each entry is {min, max, owningShard}; unowned ranges are left as real gaps.
    // `collectionShardId` is the shard whose perspective `keyBelongsToMe` answers from.
    static CollectionMetadata makeGappedShardedMetadata(
        OperationContext* opCtx,
        const UUID& uuid,
        const std::vector<std::tuple<int, int, ShardHandle>>& ownedChunks,
        ShardHandle collectionShardHandle = kThisShardHandle) {
        const OID epoch = OID::gen();
        const Timestamp timestamp(Date_t::now());

        // Sleep to guarantee a distinct timestamp from any other metadata built in the same test.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        std::vector<ChunkType> chunks;
        ChunkVersion version({epoch, timestamp}, {1, 0});
        for (const auto& [minVal, maxVal, owner] : ownedChunks) {
            ChunkType chunk(uuid,
                            ChunkRange(BSON(kShardKey << minVal), BSON(kShardKey << maxVal)),
                            version,
                            owner.toShardRef(opCtx));
            chunk.setOnCurrentShardSince(Timestamp(100, 0));
            chunk.setHistory({ChunkHistory(*chunk.getOnCurrentShardSince(), chunk.getShard())});
            chunks.push_back(std::move(chunk));
            version.incMajor();
        }

        CurrentChunkManager cm(makeStandaloneRoutingTableHistory(
            RoutingTableHistory::makeNewAllowingGaps(kTestNss,
                                                     uuid,
                                                     kShardKeyPattern,
                                                     false, /* unsplittable */
                                                     nullptr,
                                                     false,
                                                     epoch,
                                                     timestamp,
                                                     boost::none /* timeseriesFields */,
                                                     boost::none /* reshardingFields */,
                                                     true,
                                                     chunks)));
        return CollectionMetadata(std::move(cm), std::move(collectionShardHandle));
    }

    // Builds a delta of changed chunks to feed CollectionMetadata::makeUpdated, bumping the
    // collection placement version once per chunk so each carries a strictly newer version.
    static std::vector<ChunkType> makeChangedChunks(
        OperationContext* opCtx,
        const CollectionMetadata& metadata,
        const std::vector<std::tuple<int, int, ShardHandle>>& changedChunks) {
        auto version = metadata.getCollPlacementVersion();
        std::vector<ChunkType> result;
        for (const auto& [minVal, maxVal, owner] : changedChunks) {
            version.incMajor();
            ChunkType chunk(metadata.getUUID(),
                            ChunkRange(BSON(kShardKey << minVal), BSON(kShardKey << maxVal)),
                            version,
                            owner.toShardRef(opCtx));
            chunk.setOnCurrentShardSince(Timestamp(200, 0));
            chunk.setHistory({ChunkHistory(*chunk.getOnCurrentShardSince(), chunk.getShard())});
            result.push_back(std::move(chunk));
        }
        return result;
    }

    uint64_t getNumMetadataManagerChanges(CollectionShardingRuntime& csr) {
        return csr._numMetadataManagerChanges;
    }

    MetadataManager* getMetadataManager(CollectionShardingRuntime& csr) {
        return csr._metadataManager.get();
    }

    // Whether the CSR considers the collection tracked (its metadata carries a routing table).
    // Kept as a predicate so the private MetadataType enum is only named inside this friend
    // fixture.
    bool isMetadataTracked(CollectionShardingRuntime& csr) {
        return csr._metadataType == CollectionShardingRuntime::MetadataType::kTracked;
    }

    static repl::OplogEntry makeInvalidateCollectionMetadataOplogEntry(const NamespaceString& nss,
                                                                       const UUID& uuid) {
        // Fixed OpTime used for synthetic oplog entries. These tests should never read it because
        // CollectionCacheRecoverer is not installed, so reusing the same value everywhere is fine.
        return repl::makeCommandOplogEntry(repl::OpTime(Timestamp(1, 1), 1),
                                           nss.getCommandNS(),
                                           BSON("invalidateCollectionMetadata" << nss.coll()),
                                           boost::none,
                                           uuid);
    }

    static repl::OplogEntry makeCollectionShardingStateDeltaOplogEntry(
        const NamespaceString& nss, const UUID& uuid, const std::vector<ChunkType>& changedChunks) {
        BSONArrayBuilder changedChunksBuilder;
        for (const auto& chunk : changedChunks) {
            changedChunksBuilder.append(chunk.toConfigBSON());
        }

        // Fixed OpTime used for synthetic oplog entries. These tests should never read it because
        // CollectionCacheRecoverer is not installed, so reusing the same value everywhere is fine.
        return repl::makeCommandOplogEntry(repl::OpTime(Timestamp(1, 1), 1),
                                           nss,
                                           BSON("applyCollectionShardingStateDelta"
                                                << nss.coll() << "changedChunks"
                                                << changedChunksBuilder.arr()),
                                           boost::none,
                                           uuid);
    }
};

/**
 * Runs the given callback function within a transaction with the given placementConflictTime.
 */
template <typename Callable>
void runWithinTxn(OperationContext* opCtx,
                  boost::optional<LogicalTime> placementConflictTime,
                  Callable&& func) {
    TxnNumber txnNumber{0};

    opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx->setTxnNumber(txnNumber);
    opCtx->setInMultiDocumentTransaction();

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);

    TransactionRuntimeContext transactionRuntimeContext;
    transactionRuntimeContext.setPlacementConflictTime(placementConflictTime);

    txnParticipant.beginOrContinue(opCtx,
                                   {txnNumber},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart,
                                   transactionRuntimeContext);

    txnParticipant.unstashTransactionResources(opCtx, "DummyCommandName");
    func();
    txnParticipant.commitUnpreparedTransaction(opCtx);
    txnParticipant.stashTransactionResources(opCtx);

    opCtx->resetMultiDocumentTransactionState();
}

TEST_F(CollectionShardingRuntimeTest,
       GetCollectionDescriptionThrowsStaleConfigBeforeSetFilteringMetadataIsCalledAndNoOSSSet) {
    OperationContext* opCtx = operationContext();
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    ASSERT_FALSE(csr.getCollectionDescription(opCtx).isSharded());
    auto metadata = makeShardedMetadata(opCtx);
    ScopedSetShardRole scopedSetShardRole{
        opCtx, kTestNss, ShardVersionFactory::make(metadata), boost::none /* databaseVersion */};
    ASSERT_THROWS_CODE(csr.getCollectionDescription(opCtx), DBException, ErrorCodes::StaleConfig);
}

TEST_F(
    CollectionShardingRuntimeTest,
    GetCollectionDescriptionReturnsUnshardedAfterSetFilteringMetadataIsCalledWithUnshardedMetadata) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    csr.setCollectionMetadata(operationContext(), CollectionMetadata::UNTRACKED());
    ASSERT_FALSE(csr.getCollectionDescription(operationContext()).isSharded());
}

TEST_F(CollectionShardingRuntimeTest,
       GetCollectionDescriptionReturnsShardedAfterSetFilteringMetadataIsCalledWithShardedMetadata) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);
    csr.setCollectionMetadata(opCtx, metadata);
    ScopedSetShardRole scopedSetShardRole{
        opCtx, kTestNss, ShardVersionFactory::make(metadata), boost::none /* databaseVersion */};
    ASSERT_TRUE(csr.getCollectionDescription(opCtx).isSharded());
}

TEST_F(CollectionShardingRuntimeTest, CheckShardVersionThrowsStaleConfigAfterDonatingLastChunk) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();

    // This shard ("0") owns the collection's only chunk.
    auto metadata = makeShardedMetadata(opCtx, UUID::gen(), kThisShardHandle, kThisShardHandle);

    // The version a stale router would still send after the chunk has moved away.
    const auto staleShardVersion = ShardVersionFactory::make(metadata);

    // Donate the last chunk to another shard through the authoritative delta path. The donor now
    // owns no chunks, so its shard version collapses to {generation, 0, 0}.
    auto newVersion = metadata.getCollPlacementVersion();
    newVersion.incMajor();
    ChunkType changedChunk(metadata.getUUID(),
                           ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)),
                           newVersion,
                           kOtherShardHandle.toShardRef(opCtx));
    changedChunk.setOnCurrentShardSince(Timestamp(200, 0));
    changedChunk.setHistory(
        {ChunkHistory(*changedChunk.getOnCurrentShardSince(), changedChunk.getShard())});
    auto updatedMetadata = metadata.makeUpdated({changedChunk}, kThisShardHandle);

    csr.setAuthoritative();
    csr.setCollectionMetadata(
        opCtx, updatedMetadata, CollectionShardingRuntime::NoRoutingTableAs::kUnowned);

    // A CRUD operation arriving with the pre-donation shard version is rejected as stale.
    {
        ScopedSetShardRole scopedSetShardRole{
            opCtx, kTestNss, staleShardVersion, boost::none /* databaseVersion */};
        ASSERT_THROWS_CODE(
            csr.checkShardVersionOrThrow(opCtx), DBException, ErrorCodes::StaleConfig);
    }

    // The donor now reports a {generation, 0, 0} placement version, since it owns no chunks.
    const auto postDonationShardVersion = ShardVersionFactory::make(updatedMetadata);
    ASSERT_EQ(
        postDonationShardVersion.placementVersion(),
        ChunkVersion(static_cast<CollectionGeneration>(updatedMetadata.getCollPlacementVersion()),
                     {0, 0}));

    // An operation arriving with the post-donation shard version is accepted.
    {
        ScopedSetShardRole scopedSetShardRole{
            opCtx, kTestNss, postDonationShardVersion, boost::none /* databaseVersion */};
        ASSERT_DOES_NOT_THROW(csr.checkShardVersionOrThrow(opCtx));
    }
}

TEST_F(CollectionShardingRuntimeTest,
       GetCurrentMetadataIfKnownReturnsNoneBeforeSetFilteringMetadataIsCalled) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    ASSERT_FALSE(csr.getCurrentMetadataIfKnown());
}

TEST_F(
    CollectionShardingRuntimeTest,
    GetCurrentMetadataIfKnownReturnsUnshardedAfterSetFilteringMetadataIsCalledWithUntrackedMetadata) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    csr.setCollectionMetadata(operationContext(), CollectionMetadata::UNTRACKED());
    const auto optCurrMetadata = csr.getCurrentMetadataIfKnown();
    ASSERT_TRUE(optCurrMetadata);
    ASSERT_FALSE(optCurrMetadata->isSharded());
    ASSERT_EQ(optCurrMetadata->getShardPlacementVersion(), ChunkVersion::UNTRACKED());
}

TEST_F(
    CollectionShardingRuntimeTest,
    GetCurrentMetadataIfKnownReturnsShardedAfterSetFilteringMetadataIsCalledWithShardedMetadata) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);
    csr.setCollectionMetadata(opCtx, metadata);
    const auto optCurrMetadata = csr.getCurrentMetadataIfKnown();
    ASSERT_TRUE(optCurrMetadata);
    ASSERT_TRUE(optCurrMetadata->isSharded());
    ASSERT_EQ(optCurrMetadata->getShardPlacementVersion(), metadata.getShardPlacementVersion());
}

TEST_F(CollectionShardingRuntimeTest,
       GetCurrentMetadataIfKnownReturnsNoneAfterClearFilteringMetadataIsCalled) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    csr.setCollectionMetadata(opCtx, makeShardedMetadata(opCtx));
    csr.clearCollectionMetadata(opCtx);
    ASSERT_FALSE(csr.getCurrentMetadataIfKnown());
}

TEST_F(CollectionShardingRuntimeTest,
       ClearFilteringMetadataAuthoritativeClearsMetadataWhenTrackedAndUUIDMatches) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    auto collUuid = UUID::gen();
    csr.setCollectionMetadata(opCtx, makeShardedMetadata(opCtx, collUuid));
    ASSERT_TRUE(csr.getCurrentMetadataIfKnown());

    csr.clearCollectionMetadata(opCtx);
    csr.setAuthoritative();

    ASSERT_FALSE(csr.getCurrentMetadataIfKnown());
    ASSERT_EQ(csr.getAuthoritativeState(),
              CollectionShardingRuntime::AuthoritativeState::kAuthoritative);
}

TEST_F(CollectionShardingRuntimeTest,
       ClearFilteringMetadataAuthoritativeClearsMetadataWhenUntracked) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    csr.setCollectionMetadata(opCtx, CollectionMetadata::UNTRACKED());
    ASSERT_TRUE(csr.getCurrentMetadataIfKnown());

    csr.clearCollectionMetadata(opCtx);
    csr.setAuthoritative();

    ASSERT_FALSE(csr.getCurrentMetadataIfKnown());
    ASSERT_EQ(csr.getAuthoritativeState(),
              CollectionShardingRuntime::AuthoritativeState::kAuthoritative);
}

TEST_F(CollectionShardingRuntimeTest,
       ClearFilteringMetadataAuthoritativeClearsMetadataWhenUnknown) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    ASSERT_FALSE(csr.getCurrentMetadataIfKnown());

    csr.clearCollectionMetadata(opCtx);
    csr.setAuthoritative();

    ASSERT_FALSE(csr.getCurrentMetadataIfKnown());
    ASSERT_EQ(csr.getAuthoritativeState(),
              CollectionShardingRuntime::AuthoritativeState::kAuthoritative);
}

TEST_F(CollectionShardingRuntimeTest, SetCollectionMetadataKeepsNonAuthoritativeState) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);

    ASSERT_EQ(csr.getAuthoritativeState(),
              CollectionShardingRuntime::AuthoritativeState::kNonAuthoritative);

    csr.setCollectionMetadata(opCtx, metadata);

    ASSERT_TRUE(csr.getCurrentMetadataIfKnown());
    ASSERT_EQ(csr.getAuthoritativeState(),
              CollectionShardingRuntime::AuthoritativeState::kNonAuthoritative);
}

TEST_F(CollectionShardingRuntimeTest, SetCollectionMetadataKeepsAuthoritativeState) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);

    csr.clearCollectionMetadata(opCtx);
    csr.setAuthoritative();
    ASSERT_EQ(csr.getAuthoritativeState(),
              CollectionShardingRuntime::AuthoritativeState::kAuthoritative);

    csr.setCollectionMetadata(opCtx, metadata);

    ASSERT_TRUE(csr.getCurrentMetadataIfKnown());
    ASSERT_EQ(csr.getAuthoritativeState(),
              CollectionShardingRuntime::AuthoritativeState::kAuthoritative);
}

TEST_F(CollectionShardingRuntimeTest, ClearCollectionMetadataKeepsNonAuthoritativeState) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    csr.setCollectionMetadata(opCtx, makeShardedMetadata(opCtx));

    csr.clearCollectionMetadata(opCtx);

    ASSERT_FALSE(csr.getCurrentMetadataIfKnown());
    ASSERT_EQ(csr.getAuthoritativeState(),
              CollectionShardingRuntime::AuthoritativeState::kNonAuthoritative);
}

TEST_F(CollectionShardingRuntimeTest, ClearCollectionMetadataKeepsAuthoritativeState) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    csr.setCollectionMetadata(
        opCtx, makeShardedMetadata(opCtx), CollectionShardingRuntime::NoRoutingTableAs::kUntracked);
    csr.setAuthoritative();

    csr.clearCollectionMetadata(opCtx);

    ASSERT_FALSE(csr.getCurrentMetadataIfKnown());
    ASSERT_EQ(csr.getAuthoritativeState(),
              CollectionShardingRuntime::AuthoritativeState::kAuthoritative);
}

TEST_F(CollectionShardingRuntimeTest, SetAuthoritativeUpdatesAuthoritativeStateOnly) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    ASSERT_EQ(csr.getAuthoritativeState(),
              CollectionShardingRuntime::AuthoritativeState::kNonAuthoritative);
    ASSERT_FALSE(csr.getCurrentMetadataIfKnown());

    csr.setAuthoritative();

    ASSERT_EQ(csr.getAuthoritativeState(),
              CollectionShardingRuntime::AuthoritativeState::kAuthoritative);
    ASSERT_FALSE(csr.getCurrentMetadataIfKnown());
}

TEST_F(CollectionShardingRuntimeTest, SetFilteringMetadataWithSameUUIDKeepsSameMetadataManager) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    ASSERT_EQ(getNumMetadataManagerChanges(csr), 0);
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);
    csr.setCollectionMetadata(opCtx, metadata);
    // Should create a new MetadataManager object, bumping the count to 1.
    ASSERT_EQ(getNumMetadataManagerChanges(csr), 1);
    // Set it again.
    csr.setCollectionMetadata(opCtx, metadata);
    // Should not have reset metadata, so the counter should still be 1.
    ASSERT_EQ(getNumMetadataManagerChanges(csr), 1);
}

TEST_F(CollectionShardingRuntimeTest,
       SetFilteringMetadataWithDifferentUUIDReplacesPreviousMetadataManager) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);
    csr.setCollectionMetadata(opCtx, metadata);
    ScopedSetShardRole scopedSetShardRole{
        opCtx, kTestNss, ShardVersionFactory::make(metadata), boost::none /* databaseVersion */};
    ASSERT_EQ(getNumMetadataManagerChanges(csr), 1);

    // Set it again with a different metadata object (UUID is generated randomly in
    // makeShardedMetadata()).
    auto newMetadata = makeShardedMetadata(opCtx);
    csr.setCollectionMetadata(opCtx, newMetadata);

    ASSERT_EQ(getNumMetadataManagerChanges(csr), 2);
    ASSERT(
        csr.getCollectionDescription(opCtx).uuidMatches(newMetadata.getChunkManager()->getUUID()));
}

TEST_F(CollectionShardingRuntimeTest,
       TransitioningFromUntrackedToTrackedMetadataKeepsSameMetadataManager) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    ASSERT_EQ(0, getNumMetadataManagerChanges(csr));

    // Set an UNTRACKED metadata
    csr.setCollectionMetadata(opCtx, CollectionMetadata::UNTRACKED());

    // Should create a new MetadataManager object, bumping the count to 1.
    ASSERT_EQ(1, getNumMetadataManagerChanges(csr));
    ASSERT_EQ(0, getMetadataManager(csr)->numberOfMetadataSnapshots());
    ASSERT_EQ(boost::none, getMetadataManager(csr)->getCollectionUuid());
    ASSERT_EQ(false, getMetadataManager(csr)->hasRoutingTable());

    // Retain the added metadata when a newer metadata is installed
    auto rangePreserver = csr.getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);

    // Set a TRACKED METADATA
    auto metadata = makeShardedMetadata(opCtx);
    csr.setCollectionMetadata(opCtx, metadata);

    // Should not have reset metadata, so the counter should still be 1.
    ASSERT_EQ(1, getNumMetadataManagerChanges(csr));
    ASSERT_EQ(1, getMetadataManager(csr)->numberOfMetadataSnapshots());
    ASSERT_EQ(metadata.getUUID(), getMetadataManager(csr)->getCollectionUuid().get());
    ASSERT_EQ(true, getMetadataManager(csr)->hasRoutingTable());
}

TEST_F(CollectionShardingRuntimeTest,
       TransitioningFromUntrackedToUntrackedMetadataKeepsMetadataManager) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    ASSERT_EQ(0, getNumMetadataManagerChanges(csr));

    // Set an UNTRACKED metadata
    csr.setCollectionMetadata(opCtx, CollectionMetadata::UNTRACKED());

    // When a range preserver isn't bound to a metadata tracker, it gets automatically removed once
    // another filtering metadata is added. Hence, we should install a range preserver to avoid
    // getting a false positive result on this test.
    auto rangePreserver = csr.getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);

    // Should create a new MetadataManager object, bumping the count to 1.
    ASSERT_EQ(1, getNumMetadataManagerChanges(csr));
    ASSERT_EQ(0, getMetadataManager(csr)->numberOfMetadataSnapshots());
    ASSERT_EQ(boost::none, getMetadataManager(csr)->getCollectionUuid());
    ASSERT_EQ(false, getMetadataManager(csr)->hasRoutingTable());

    // Set UNTRACKED METADATA again
    csr.setCollectionMetadata(opCtx, CollectionMetadata::UNTRACKED());

    // Should not have reset metadata, so the counter should still be 1.
    // Should not have added any snapshot to the metadata manager.
    ASSERT_EQ(1, getNumMetadataManagerChanges(csr));
    ASSERT_EQ(0, getMetadataManager(csr)->numberOfMetadataSnapshots());
    ASSERT_EQ(boost::none, getMetadataManager(csr)->getCollectionUuid());
    ASSERT_EQ(false, getMetadataManager(csr)->hasRoutingTable());
}

TEST_F(CollectionShardingRuntimeTest,
       TransitioningFromTrackedToUntrackedMetadataRestoresMetadataManager) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    ASSERT_EQ(0, getNumMetadataManagerChanges(csr));

    // Set a TRACKED METADATA
    auto metadata = makeShardedMetadata(opCtx);
    csr.setCollectionMetadata(opCtx, metadata);

    // Should create a new MetadataManager object, bumping the count to 1.
    ASSERT_EQ(1, getNumMetadataManagerChanges(csr));
    ASSERT_EQ(0, getMetadataManager(csr)->numberOfMetadataSnapshots());
    ASSERT_EQ(metadata.getUUID(), getMetadataManager(csr)->getCollectionUuid().get());
    ASSERT_EQ(true, getMetadataManager(csr)->hasRoutingTable());

    // Set UNTRACKED METADATA
    csr.setCollectionMetadata(opCtx, CollectionMetadata::UNTRACKED());

    // Should have reset metadata, so the counter should have bumped to 1.
    ASSERT_EQ(2, getNumMetadataManagerChanges(csr));
    ASSERT_EQ(0, getMetadataManager(csr)->numberOfMetadataSnapshots());
    ASSERT_EQ(boost::none, getMetadataManager(csr)->getCollectionUuid());
    ASSERT_EQ(false, getMetadataManager(csr)->hasRoutingTable());
}

TEST_F(CollectionShardingRuntimeTest,
       TransitioningFromTrackedToTrackedMetadataWithSameUUIDKeepsMetadataManager) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    ASSERT_EQ(0, getNumMetadataManagerChanges(csr));

    auto metadata1 = makeShardedMetadata(opCtx);
    auto metadata2 = makeShardedMetadata(opCtx, metadata1.getUUID());

    // Set a TRACKED METADATA
    {
        csr.setCollectionMetadata(opCtx, metadata1);

        // Should create a new MetadataManager object, bumping the count to 1.
        ASSERT_EQ(1, getNumMetadataManagerChanges(csr));
        ASSERT_EQ(0, getMetadataManager(csr)->numberOfMetadataSnapshots());
        ASSERT_EQ(metadata1.getUUID(), getMetadataManager(csr)->getCollectionUuid().get());
        ASSERT_EQ(true, getMetadataManager(csr)->hasRoutingTable());
    }

    // Retain the added metadata when a newer metadata is installed
    auto rangePreserver = csr.getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);

    // Set a TRACKED METADATA again with the same UUID
    {
        csr.setCollectionMetadata(opCtx, metadata2);

        // Should keep the same MetadataManager object and increase the number of snapshots
        ASSERT_EQ(1, getNumMetadataManagerChanges(csr));
        ASSERT_EQ(1, getMetadataManager(csr)->numberOfMetadataSnapshots());
        ASSERT_EQ(metadata1.getUUID(), getMetadataManager(csr)->getCollectionUuid().get());
        ASSERT_EQ(true, getMetadataManager(csr)->hasRoutingTable());
    }
}

TEST_F(CollectionShardingRuntimeTest,
       TransitioningFromTrackedToTrackedMetadataWithDifferentUUIDRestoresMetadataManager) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    ASSERT_EQ(0, getNumMetadataManagerChanges(csr));

    const auto metadata1 = makeShardedMetadata(opCtx);
    const auto metadata2 = makeShardedMetadata(opCtx);
    ASSERT_NE(metadata1.getUUID(), metadata2.getUUID());

    // Set a TRACKED METADATA
    {
        csr.setCollectionMetadata(opCtx, metadata1);

        // Should create a new MetadataManager object, bumping the count to 1.
        ASSERT_EQ(1, getNumMetadataManagerChanges(csr));
        ASSERT_EQ(0, getMetadataManager(csr)->numberOfMetadataSnapshots());
        ASSERT_EQ(metadata1.getUUID(), getMetadataManager(csr)->getCollectionUuid().get());
        ASSERT_EQ(true, getMetadataManager(csr)->hasRoutingTable());
    }

    // Set a TRACKED METADATA again with a different UUID
    {
        csr.setCollectionMetadata(opCtx, metadata2);

        // Should restore the MetadataManager object.
        ASSERT_EQ(2, getNumMetadataManagerChanges(csr));
        ASSERT_EQ(0, getMetadataManager(csr)->numberOfMetadataSnapshots());
        ASSERT_EQ(metadata2.getUUID(), getMetadataManager(csr)->getCollectionUuid().get());
        ASSERT_EQ(true, getMetadataManager(csr)->hasRoutingTable());
    }
}


TEST_F(CollectionShardingRuntimeTest, ShardVersionCheckDetectsClusterTimeConflicts) {
    OperationContext* opCtx = operationContext();
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    const auto metadata = makeShardedMetadata(opCtx);
    csr.setCollectionMetadata(opCtx, metadata);

    const auto collectionTimestamp = metadata.getShardPlacementVersion().getTimestamp();

    const auto receivedShardVersion = ShardVersionFactory::make(metadata);

    // Test that conflict is thrown when transaction 'atClusterTime' is not valid the current shard
    // version.
    {
        const auto previousReadConcern = repl::ReadConcernArgs::get(operationContext());
        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);

        // Valid atClusterTime (equal or later than collection timestamp).
        {
            repl::ReadConcernArgs::get(operationContext())
                .setArgsAtClusterTimeForSnapshot(collectionTimestamp + 1);
            ScopedSetShardRole scopedSetShardRole{
                opCtx, kTestNss, receivedShardVersion, boost::none /* databaseVersion */};
            ASSERT_DOES_NOT_THROW(csr.checkShardVersionOrThrow(opCtx));
        }

        // Conflicting atClusterTime (earlier than collection timestamp).
        repl::ReadConcernArgs::get(operationContext())
            .setArgsAtClusterTimeForSnapshot(collectionTimestamp - 1);
        ScopedSetShardRole scopedSetShardRole{
            opCtx, kTestNss, receivedShardVersion, boost::none /* databaseVersion */};
        ASSERT_THROWS_CODE(
            csr.checkShardVersionOrThrow(opCtx), DBException, ErrorCodes::SnapshotUnavailable);

        repl::ReadConcernArgs::get(operationContext()) = previousReadConcern;
    }

    // Test that conflict is thrown when transaction 'placementConflictTime' is not valid the
    // current shard version, when the ff AddTransactionRuntimeContextAsAGenericArgument is
    // enabled, meaning that the placementConflictTime is retrieved from the TransactionParticipant.
    {
        unittest::ServerParameterGuard featureFlagController(
            "featureFlagAddTransactionRuntimeContextAsAGenericArgument", true);

        runWithinTxn(operationContext(), LogicalTime(collectionTimestamp + 1), [&]() {
            ScopedSetShardRole scopedSetShardRole{
                opCtx, kTestNss, receivedShardVersion, boost::none /* databaseVersion */};
            ASSERT_DOES_NOT_THROW(csr.checkShardVersionOrThrow(opCtx));
        });

        runWithinTxn(operationContext(), LogicalTime(collectionTimestamp - 1), [&]() {
            ScopedSetShardRole scopedSetShardRole{
                opCtx, kTestNss, receivedShardVersion, boost::none /* databaseVersion */};
            ASSERT_THROWS_CODE(
                csr.checkShardVersionOrThrow(opCtx), DBException, ErrorCodes::SnapshotUnavailable);
        });

        // The placementConflictTime attached to the DatabaseVersion should be ignored if the
        // featureFlagAddTransactionRuntimeContextAsAGenericArgument is enabled.
        {
            auto receivedShardVersionWithPCT = receivedShardVersion;
            receivedShardVersionWithPCT.setPlacementConflictTime_DEPRECATED(
                LogicalTime(collectionTimestamp - 1));
            ScopedSetShardRole scopedSetShardRole{
                opCtx, kTestNss, receivedShardVersionWithPCT, boost::none /* databaseVersion */};
            ASSERT_DOES_NOT_THROW(csr.checkShardVersionOrThrow(opCtx));
        }
    }

    // Test that conflict is thrown when transaction 'placementConflictTime' is not valid the
    // current shard version, when the ff AddTransactionRuntimeContextAsAGenericArgument is
    // disabled, meaning that the placementConflictTime is retrieved from the ShardVersion object.
    {
        unittest::ServerParameterGuard featureFlagController(
            "featureFlagAddTransactionRuntimeContextAsAGenericArgument", false);

        // Valid placementConflictTime (equal or later than collection timestamp).
        {
            auto receivedShardVersionWithPCT = receivedShardVersion;
            receivedShardVersionWithPCT.setPlacementConflictTime_DEPRECATED(
                LogicalTime(collectionTimestamp + 1));
            ScopedSetShardRole scopedSetShardRole{
                opCtx, kTestNss, receivedShardVersionWithPCT, boost::none /* databaseVersion */};
            ASSERT_DOES_NOT_THROW(csr.checkShardVersionOrThrow(opCtx));
        }

        // Conflicting placementConflictTime (earlier than collection timestamp).
        {
            auto receivedShardVersionWithPCT = receivedShardVersion;
            receivedShardVersionWithPCT.setPlacementConflictTime_DEPRECATED(
                LogicalTime(collectionTimestamp - 1));
            ScopedSetShardRole scopedSetShardRole{
                opCtx, kTestNss, receivedShardVersionWithPCT, boost::none /* databaseVersion */};
            ASSERT_THROWS_CODE(
                csr.checkShardVersionOrThrow(opCtx), DBException, ErrorCodes::SnapshotUnavailable);
        }
    }
}

using CollectionShardingRuntimeTestDeathTest = CollectionShardingRuntimeTest;

DEATH_TEST_REGEX_F(CollectionShardingRuntimeTestDeathTest,
                   TestsShouldTassertIfPlacementConflictTimeIsNotPresentInTxns,
                   "Tripwire assertion.*10206300") {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    const auto metadata = makeShardedMetadata(operationContext());
    csr.setCollectionMetadata(operationContext(), metadata);
    const auto receivedShardVersion = ShardVersionFactory::make(metadata);

    runWithinTxn(operationContext(), boost::none, [&]() {
        ScopedSetShardRole scopedSetShardRole{
            operationContext(), kTestNss, receivedShardVersion, boost::none /* databaseVersion */};
        csr.checkShardVersionOrThrow(operationContext());
    });
}

TEST_F(CollectionShardingRuntimeTest, InvalidateRangePreserversOlderThanShardVersion) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    // By default, makeShardedMetadata assigns different shard IDs to the chunk ("otherShard") and
    // the collection ("thisShard"), resulting in a placement version of {0,0}. For this test, we
    // want to ensure the chunk and collection share the same shard ID ("thisShard") to generate
    // comparable chunk versions. This setup is required to correctly test
    // invalidateRangePreserversOlderThanShardVersion, which compares shard placement versions for
    // invalidation.
    auto collectionUUID = UUID::gen();
    auto metadataInThePast =
        makeShardedMetadata(opCtx, collectionUUID, kThisShardHandle, kThisShardHandle);
    auto metadata = makeShardedMetadata(opCtx, collectionUUID, kThisShardHandle, kThisShardHandle);
    csr.setCollectionMetadata(opCtx, metadata);
    const auto optCurrMetadata = csr.getCurrentMetadataIfKnown();
    ASSERT_TRUE(optCurrMetadata);
    ASSERT_TRUE(optCurrMetadata->isSharded());
    ASSERT_EQ(optCurrMetadata->getShardPlacementVersion(), metadata.getShardPlacementVersion());

    auto ownershipFilter = csr.getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);

    ASSERT_TRUE(ownershipFilter.isRangePreserverStillValid());

    // Test that the trackers will not be invalidated with version ChunkVersion::IGNORED()
    csr.invalidateRangePreserversOlderThanShardVersion(ChunkVersion::IGNORED(), collectionUUID);
    ASSERT_TRUE(ownershipFilter.isRangePreserverStillValid());

    // Test that the trackers will not be invalidated with version which is older
    csr.invalidateRangePreserversOlderThanShardVersion(metadataInThePast.getShardPlacementVersion(),
                                                       collectionUUID);
    ASSERT_TRUE(ownershipFilter.isRangePreserverStillValid());

    // Test that the trackers will not be invalidated when collection UUID does not match
    csr.invalidateRangePreserversOlderThanShardVersion(metadata.getShardPlacementVersion(),
                                                       UUID::gen());
    ASSERT_TRUE(ownershipFilter.isRangePreserverStillValid());

    // Test that the trackers will be invalidated with current version
    csr.invalidateRangePreserversOlderThanShardVersion(metadata.getShardPlacementVersion(),
                                                       collectionUUID);
    ASSERT_FALSE(ownershipFilter.isRangePreserverStillValid());
}

TEST_F(CollectionShardingRuntimeTest, InvalidateRangePreserversOlderThanUnshardedVersion) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    auto collectionUUID = UUID::gen();
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx, collectionUUID);
    csr.setCollectionMetadata(opCtx, metadata);

    auto ownershipFilter = csr.getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);

    ASSERT_TRUE(ownershipFilter.isRangePreserverStillValid());

    // Test that the trackers will be invalidated with version ChunkVersion::UNTRACKED().
    // Test is prepared for the case when UNTRACKED metadata will be started to be tracked. In this
    // case ownershipFilter::shardPlacementVersion = UNTRACKED. Currently it's not possible to test
    // as in this case metadataManager is not created for unsharded collection. When it will be
    // changed it will be possible to test against a current version.
    csr.invalidateRangePreserversOlderThanShardVersion(ChunkVersion::UNTRACKED(), collectionUUID);
    ASSERT_FALSE(ownershipFilter.isRangePreserverStillValid());
}

TEST_F(CollectionShardingRuntimeTest, InvalidateRangePreserversUntrackedCollection) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    auto collectionUUID = UUID::gen();
    OperationContext* opCtx = operationContext();
    csr.setCollectionMetadata(opCtx, CollectionMetadata::UNTRACKED());

    auto ownershipFilter = csr.getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);

    ASSERT_TRUE(ownershipFilter.isRangePreserverStillValid());

    // Promote the collection to a sharded collection since it only make sense to invalidat range
    // preservers on sharded collections.
    const auto metadata = makeShardedMetadata(opCtx, collectionUUID);
    csr.setCollectionMetadata(opCtx, metadata);

    ASSERT_TRUE(ownershipFilter.isRangePreserverStillValid());

    csr.invalidateRangePreserversOlderThanShardVersion(metadata.getShardPlacementVersion(),
                                                       collectionUUID);
    ASSERT_FALSE(ownershipFilter.isRangePreserverStillValid());
}

TEST_F(CollectionShardingRuntimeTest, WaiterFunctionalityWorksWithCSRStateChanges) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();

    auto future = csr.registerWaiterForChunkVersion(
        opCtx, ShardVersionFactory::make(ChunkVersion::UNTRACKED()));

    ASSERT_FALSE(future.isReady());

    csr.setCollectionMetadata(opCtx, CollectionMetadata::UNTRACKED());

    ASSERT_TRUE(future.isReady());

    future = csr.registerWaiterForChunkVersion(
        opCtx, ShardVersionFactory::make(ChunkVersion::UNTRACKED()));

    ASSERT_TRUE(future.isReady());
}

TEST_F(CollectionShardingRuntimeTest, MultipleWaiterFunctionalityWorksWithCSRStateChanges) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();

    auto future1 = csr.registerWaiterForChunkVersion(
        opCtx, ShardVersionFactory::make(ChunkVersion::UNTRACKED()));
    auto future2 = csr.registerWaiterForChunkVersion(
        opCtx, ShardVersionFactory::make(ChunkVersion::UNTRACKED()));
    auto future3 = csr.registerWaiterForChunkVersion(
        opCtx, ShardVersionFactory::make(ChunkVersion::UNTRACKED()));

    ASSERT_FALSE(future1.isReady());
    ASSERT_FALSE(future2.isReady());
    ASSERT_FALSE(future3.isReady());

    csr.setCollectionMetadata(opCtx, CollectionMetadata::UNTRACKED());

    ASSERT_TRUE(future1.isReady());
    ASSERT_TRUE(future2.isReady());
    ASSERT_TRUE(future3.isReady());
}

TEST_F(CollectionShardingRuntimeTest, VersionWaiterAlsoWaitsForCriticalSectionRelease) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();

    csr.enterCriticalSectionCatchUpPhase(opCtx, BSONObj());
    csr.enterCriticalSectionCommitPhase(opCtx, BSONObj());

    auto future = csr.registerWaiterForChunkVersion(
        opCtx, ShardVersionFactory::make(ChunkVersion::UNTRACKED()));

    ASSERT_FALSE(future.isReady());

    csr.setCollectionMetadata(opCtx, CollectionMetadata::UNTRACKED());

    sleepmillis(200);

    ASSERT_FALSE(future.isReady());

    csr.exitCriticalSection(opCtx, BSONObj());

    future.get();
}

TEST_F(CollectionShardingRuntimeTest, WaiterFunctionalityWakesEarlierVersions) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();

    auto collMetadata = makeShardedMetadata(opCtx);

    auto targetVersion = collMetadata.getCollPlacementVersion();

    auto future =
        csr.registerWaiterForChunkVersion(opCtx, ShardVersionFactory::make(targetVersion));

    ASSERT_FALSE(future.isReady());

    csr.setCollectionMetadata(opCtx, CollectionMetadata::UNTRACKED());

    ASSERT_FALSE(future.isReady());

    auto newVersion = targetVersion;
    newVersion.incMajor();
    ASSERT_EQ(targetVersion <=> newVersion, std::partial_ordering::less);

    auto newMetadata = changeShardVersion(opCtx, collMetadata, newVersion);
    csr.setCollectionMetadata(opCtx, std::move(newMetadata));

    // Waiter should now be woken as it waited on a previous version.
    ASSERT_TRUE(future.isReady());
    future.get();
}

TEST_F(CollectionShardingRuntimeTest, WaiterIsWokenForMatchingUntrackedAuthoritative) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();

    ASSERT_EQ(ChunkVersion::UNTRACKED() <=> ChunkVersion::UNTRACKED(),
              std::partial_ordering::unordered);

    auto future = csr.registerWaiterForChunkVersion(
        opCtx, ShardVersionFactory::make(ChunkVersion::UNTRACKED()));

    ASSERT_FALSE(future.isReady());

    csr.setCollectionMetadata(opCtx,
                              CollectionMetadata::UNTRACKED(),
                              CollectionShardingRuntime::NoRoutingTableAs::kUntracked);
    csr.setAuthoritative();

    ASSERT_TRUE(future.isReady());
    future.get();
}

TEST_F(CollectionShardingRuntimeTest, WaiterIsWokenForMatchingTrackedZeroChunksAuthoritative) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();

    auto metadata = makeShardedMetadata(opCtx);
    auto trackedZeroChunks = metadata.getShardPlacementVersion();
    ASSERT_EQ(trackedZeroChunks.majorVersion(), 0u);
    ASSERT_EQ(trackedZeroChunks.minorVersion(), 0u);
    ASSERT_NE(trackedZeroChunks, ChunkVersion::UNTRACKED());

    ASSERT_EQ(trackedZeroChunks <=> trackedZeroChunks, std::partial_ordering::unordered);

    auto future =
        csr.registerWaiterForChunkVersion(opCtx, ShardVersionFactory::make(trackedZeroChunks));

    ASSERT_FALSE(future.isReady());

    csr.setCollectionMetadata(
        opCtx, metadata, CollectionShardingRuntime::NoRoutingTableAs::kUntracked);
    csr.setAuthoritative();

    ASSERT_TRUE(future.isReady());
    future.get();
}

// An authoritative CSR that owns no chunks yet (its routing table is all gaps) receives its first
// chunks assigned to this shard; afterwards the shard owns exactly those ranges.
TEST_F(CollectionShardingRuntimeTest, AuthoritativeEmptyCsrReceivesFirstChunks) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    const auto uuid = UUID::gen();

    // Start authoritative with zero owned chunks: every key is a gap.
    auto metadata = makeGappedShardedMetadata(opCtx, uuid, {}, kThisShardHandle);
    csr.setAuthoritative();
    csr.setCollectionMetadata(
        opCtx, metadata, CollectionShardingRuntime::NoRoutingTableAs::kUnowned);

    {
        auto filter = csr.getOwnershipFilter(
            opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);
        ASSERT_FALSE(filter.keyBelongsToMe(BSON(kShardKey << 5)));
    }

    // Receive the first chunks, both assigned to this shard.
    auto updated = metadata.makeUpdated(
        makeChangedChunks(opCtx, metadata, {{0, 10, kThisShardHandle}, {20, 30, kThisShardHandle}}),
        kThisShardHandle);
    csr.setAuthoritative();
    csr.setCollectionMetadata(
        opCtx, updated, CollectionShardingRuntime::NoRoutingTableAs::kUnowned);

    auto filter = csr.getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);
    ASSERT_TRUE(csr.getCurrentMetadataIfKnown()->isSharded());
    ASSERT(isMetadataTracked(csr));
    ASSERT_TRUE(filter.keyBelongsToMe(BSON(kShardKey << 5)));
    ASSERT_TRUE(filter.keyBelongsToMe(BSON(kShardKey << 25)));
    ASSERT_FALSE(filter.keyBelongsToMe(BSON(kShardKey << 15)));  // interior gap
}

// An authoritative CSR receives new chunks that are assigned to another shard; this shard does not
// own them, but the placement version still advances.
TEST_F(CollectionShardingRuntimeTest, AuthoritativeCsrReceivesChunksForAnotherShard) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    const auto uuid = UUID::gen();

    auto metadata =
        makeGappedShardedMetadata(opCtx, uuid, {{0, 10, kThisShardHandle}}, kThisShardHandle);
    csr.setAuthoritative();
    csr.setCollectionMetadata(
        opCtx, metadata, CollectionShardingRuntime::NoRoutingTableAs::kUnowned);

    auto updated = metadata.makeUpdated(
        makeChangedChunks(opCtx, metadata, {{20, 30, kOtherShardHandle}}), kThisShardHandle);
    csr.setAuthoritative();
    csr.setCollectionMetadata(
        opCtx, updated, CollectionShardingRuntime::NoRoutingTableAs::kUnowned);

    auto filter = csr.getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);
    ASSERT_TRUE(filter.keyBelongsToMe(BSON(kShardKey << 5)));    // still ours
    ASSERT_FALSE(filter.keyBelongsToMe(BSON(kShardKey << 25)));  // owned by the other shard
    ASSERT(isMetadataTracked(csr));
    ASSERT(updated.getCollPlacementVersion() <=> metadata.getCollPlacementVersion() ==
           std::partial_ordering::greater);
}

// An authoritative CSR receives new chunks assigned to this shard, extending what it owns.
TEST_F(CollectionShardingRuntimeTest, AuthoritativeCsrReceivesChunksForCurrentShard) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    const auto uuid = UUID::gen();

    auto metadata =
        makeGappedShardedMetadata(opCtx, uuid, {{0, 10, kThisShardHandle}}, kThisShardHandle);
    csr.setAuthoritative();
    csr.setCollectionMetadata(
        opCtx, metadata, CollectionShardingRuntime::NoRoutingTableAs::kUnowned);

    auto updated = metadata.makeUpdated(
        makeChangedChunks(opCtx, metadata, {{20, 30, kThisShardHandle}}), kThisShardHandle);
    csr.setAuthoritative();
    csr.setCollectionMetadata(
        opCtx, updated, CollectionShardingRuntime::NoRoutingTableAs::kUnowned);

    auto filter = csr.getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);
    ASSERT(isMetadataTracked(csr));
    ASSERT_TRUE(filter.keyBelongsToMe(BSON(kShardKey << 5)));    // original chunk
    ASSERT_TRUE(filter.keyBelongsToMe(BSON(kShardKey << 25)));   // newly received chunk
    ASSERT_FALSE(filter.keyBelongsToMe(BSON(kShardKey << 15)));  // gap between them
}

// An authoritative CSR that owns exactly one chunk donates it to another shard; afterwards the
// shard owns nothing, but the collection is still sharded.
TEST_F(CollectionShardingRuntimeTest, AuthoritativeSingleChunkCsrDonatesItsOnlyChunk) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    const auto uuid = UUID::gen();

    auto metadata =
        makeGappedShardedMetadata(opCtx, uuid, {{0, 10, kThisShardHandle}}, kThisShardHandle);
    csr.setAuthoritative();
    csr.setCollectionMetadata(
        opCtx, metadata, CollectionShardingRuntime::NoRoutingTableAs::kUnowned);

    {
        auto filter = csr.getOwnershipFilter(
            opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);
        ASSERT_TRUE(filter.keyBelongsToMe(BSON(kShardKey << 5)));
    }

    // The same range is re-tagged to another shard, so this shard owns nothing afterwards.
    auto updated = metadata.makeUpdated(
        makeChangedChunks(opCtx, metadata, {{0, 10, kOtherShardHandle}}), kThisShardHandle);
    csr.setAuthoritative();
    csr.setCollectionMetadata(
        opCtx, updated, CollectionShardingRuntime::NoRoutingTableAs::kUnowned);

    auto filter = csr.getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);
    ASSERT_TRUE(csr.getCurrentMetadataIfKnown()->isSharded());
    // The shard owns nothing now, yet the state stays kTracked because a routing table is present.
    ASSERT(isMetadataTracked(csr));
    ASSERT_FALSE(filter.keyBelongsToMe(BSON(kShardKey << 5)));  // donated away
}

// A single delta mixing a chunk for this shard and a chunk for another shard is applied atomically.
TEST_F(CollectionShardingRuntimeTest, AuthoritativeCsrReceivesMixedChunkDelta) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    const auto uuid = UUID::gen();

    auto metadata = makeGappedShardedMetadata(opCtx, uuid, {}, kThisShardHandle);
    csr.setAuthoritative();
    csr.setCollectionMetadata(
        opCtx, metadata, CollectionShardingRuntime::NoRoutingTableAs::kUnowned);

    auto updated = metadata.makeUpdated(
        makeChangedChunks(
            opCtx, metadata, {{0, 10, kThisShardHandle}, {20, 30, kOtherShardHandle}}),
        kThisShardHandle);
    csr.setAuthoritative();
    csr.setCollectionMetadata(
        opCtx, updated, CollectionShardingRuntime::NoRoutingTableAs::kUnowned);

    auto filter = csr.getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);
    ASSERT(isMetadataTracked(csr));
    ASSERT_TRUE(filter.keyBelongsToMe(BSON(kShardKey << 5)));    // assigned to this shard
    ASSERT_FALSE(filter.keyBelongsToMe(BSON(kShardKey << 25)));  // assigned to the other shard
}

class CollectionShardingRuntimeTestWithMockedLoader
    : public ShardServerTestFixtureWithCatalogCacheLoaderMock {
public:
    const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.foo");
    const UUID kCollUUID = UUID::gen();
    const std::string kShardKey = "x";
    const std::vector<ShardType> kShardList = {
        ShardType(kThisShardHandle.name().toString(), kThisShardHandle.uuid(), "Host0:12345")};

    void setUp() override {
        ShardServerTestFixtureWithCatalogCacheLoaderMock::setUp();

        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());

        for (const auto& shard : kShardList) {
            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            HostAndPort host(shard.getHost());
            targeter->setConnectionStringReturnValue(ConnectionString(host));
            targeter->setFindHostReturnValue(host);
            targeterFactory()->addTargeterToReturn(ConnectionString(host), std::move(targeter));
        }
    }

    void tearDown() override {
        WaitForMajorityService::get(getServiceContext()).shutDown();

        ShardServerTestFixtureWithCatalogCacheLoaderMock::tearDown();
    }

    class StaticCatalogClient final : public ShardingCatalogClientMock {
    public:
        StaticCatalogClient(std::vector<ShardType> shards) : _shards(std::move(shards)) {}

        repl::OpTimeWith<std::vector<ShardType>> getAllShards(OperationContext* opCtx,
                                                              repl::ReadConcernLevel readConcern,
                                                              BSONObj filter) override {
            return repl::OpTimeWith<std::vector<ShardType>>(_shards);
        }

        std::vector<CollectionType> getShardedCollections(OperationContext* opCtx,
                                                          const DatabaseName& dbName,
                                                          repl::ReadConcernLevel readConcernLevel,
                                                          const BSONObj& sort) override {
            return {};
        }

        std::vector<CollectionType> getCollections(OperationContext* opCtx,
                                                   const DatabaseName& dbName,
                                                   repl::ReadConcernLevel readConcernLevel,
                                                   const BSONObj& sort) override {
            return _colls;
        }

        void setCollections(std::vector<CollectionType> colls) {
            _colls = std::move(colls);
        }

    private:
        const std::vector<ShardType> _shards;
        std::vector<CollectionType> _colls;
    };

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        return std::make_unique<StaticCatalogClient>(kShardList);
    }

    CollectionType createCollection(const OID& epoch, const Timestamp& timestamp) {
        CollectionType res(kNss, epoch, timestamp, Date_t::now(), kCollUUID, BSON(kShardKey << 1));
        res.setAllowMigrations(false);
        return res;
    }

    std::vector<ChunkType> createChunks(OperationContext* opCtx,
                                        const OID& epoch,
                                        const UUID& uuid,
                                        const Timestamp& timestamp) {
        auto range1 = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << 5));
        ChunkType chunk1(uuid,
                         range1,
                         ChunkVersion({epoch, timestamp}, {1, 0}),
                         kShardList[0].getHandle().toShardRef(opCtx));

        auto range2 = ChunkRange(BSON(kShardKey << 5), BSON(kShardKey << MAXKEY));
        ChunkType chunk2(uuid,
                         range2,
                         ChunkVersion({epoch, timestamp}, {1, 1}),
                         kShardList[0].getHandle().toShardRef(opCtx));

        return {chunk1, chunk2};
    }
};

TEST_F(CollectionShardingRuntimeTestWithMockedLoader, CheckCriticalSectionMetricsAreReported) {
    auto acq = acquireCollection(operationContext(),
                                 CollectionAcquisitionRequest{kNss,
                                                              PlacementConcern::kPretendUnsharded,
                                                              repl::ReadConcernArgs{},
                                                              AcquisitionPrerequisites::kWrite},
                                 MODE_X);
    auto getStatistics = [&] {
        auto& shardingStatistics = ShardingStatistics::get(operationContext());
        BSONObjBuilder builder;
        shardingStatistics.report(&builder);
        auto fullMetrics = builder.obj();
        return fullMetrics.getObjectField("collectionCriticalSectionStatistics").getOwned();
    };

    const auto csr = CollectionShardingRuntime::acquireExclusive(operationContext(), kNss);

    auto metrics = getStatistics();
    ASSERT_EQ(metrics["activeCatchupCount"].safeNumberLong(), 0);
    ASSERT_EQ(metrics["activeCommitCount"].safeNumberLong(), 0);

    csr->enterCriticalSectionCatchUpPhase(operationContext(), BSONObj());

    metrics = getStatistics();
    ASSERT_EQ(metrics["activeCatchupCount"].safeNumberLong(), 1);
    ASSERT_EQ(metrics["activeCommitCount"].safeNumberLong(), 0);
    ASSERT_EQ(metrics["totalTimeActiveCommitMillis"].safeNumberLong(), 0);

    csr->enterCriticalSectionCommitPhase(operationContext(), BSONObj());

    metrics = getStatistics();
    ASSERT_EQ(metrics["activeCatchupCount"].safeNumberLong(), 0);
    ASSERT_EQ(metrics["activeCommitCount"].safeNumberLong(), 1);
    ASSERT_EQ(metrics["totalTimeActiveCatchupMillis"].safeNumberLong(), 0);

    ASSERT_EQ(metrics["totalTimeWaiting"].safeNumberLong(), 0);
    ASSERT_EQ(metrics["activeWaitersCount"].safeNumberLong(), 0);

    ASSERT_DOES_NOT_THROW(csr->exitCriticalSection(operationContext(), BSONObj()));

    metrics = getStatistics();
    ASSERT_EQ(metrics["activeCatchupCount"].safeNumberLong(), 0);
    ASSERT_EQ(metrics["activeCommitCount"].safeNumberLong(), 0);
    ASSERT_EQ(metrics["activeWaitersCount"].safeNumberLong(), 0);
    ASSERT_EQ(metrics["totalTimeWaiting"].safeNumberLong(), 0);
}

class CollectionShardingRuntimeUniqueShardIdentifiersTestWithMockedLoader
    : public CollectionShardingRuntimeTestWithMockedLoader,
      public testing::WithParamInterface<bool> {
protected:
    void setUp() override {
        _featureFlagScope.emplace("featureFlagUniqueShardIdentifiers", GetParam());
        CollectionShardingRuntimeTestWithMockedLoader::setUp();
    }

    void tearDown() override {
        CollectionShardingRuntimeTestWithMockedLoader::tearDown();
        _featureFlagScope.reset();
    }

private:
    boost::optional<unittest::ServerParameterGuard> _featureFlagScope;
};

INSTANTIATE_TEST_SUITE_P(UniqueShardIdentifiers,
                         CollectionShardingRuntimeUniqueShardIdentifiersTestWithMockedLoader,
                         testing::Bool(),
                         [](const testing::TestParamInfo<bool>& info) {
                             return info.param ? "WithUniqueShardIdentifiers"
                                               : "WithoutUniqueShardIdentifiers";
                         });

TEST_P(CollectionShardingRuntimeUniqueShardIdentifiersTestWithMockedLoader,
       CriticalSectionMetricsReportWaiters) {
    const BSONObj criticalSectionReason = BSON("reason" << 1);
    {
        // Enter the critical section.
        const auto& csr = CollectionShardingRuntime::acquireExclusive(operationContext(), kNss);
        csr->enterCriticalSectionCatchUpPhase(operationContext(), criticalSectionReason);
        csr->enterCriticalSectionCommitPhase(operationContext(), criticalSectionReason);
    }

    const ShardVersion shardVersionShardedCollection1 = ShardVersionFactory::make(ChunkVersion(
        CollectionGeneration{OID::gen(), Timestamp(5, 0)}, CollectionPlacement(10, 1)));

    auto getStatistics = [&] {
        auto& shardingStatistics = ShardingStatistics::get(operationContext());
        BSONObjBuilder builder;
        shardingStatistics.report(&builder);
        auto fullMetrics = builder.obj();
        return fullMetrics.getObjectField("collectionCriticalSectionStatistics").getOwned();
    };

    // At this point the critical section has been taken, so a new waiter will have to wait for the
    // signal to finish. This means that
    std::barrier phase(2);
    stdx::thread waiter([&] {
        PlacementConcern placementConcern{{}, shardVersionShardedCollection1};

        auto validateException = [&](const DBException& ex) {
            const auto exInfo = ex.extraInfo<StaleConfigInfo>();
            ASSERT_EQ(kNss, exInfo->getNss());
            ASSERT_EQ(shardVersionShardedCollection1, exInfo->getVersionReceived());
            ASSERT_EQ(boost::none, exInfo->getVersionWanted());
            ASSERT_EQ(kMyShardHandle.toShardRef(operationContext()), exInfo->getShardRef());
            const auto& signal = exInfo->getCriticalSectionSignal();
            sleepmillis(10);
            auto metrics = getStatistics();
            ASSERT_EQ(metrics["activeWaitersCount"].safeNumberLong(), 0);
            ASSERT_TRUE(signal.is_initialized());
            phase.arrive_and_wait();
            signal->get(operationContext());
        };
        ASSERT_THROWS_WITH_CHECK(
            acquireCollection(
                operationContext(),
                {kNss, placementConcern, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
                MODE_IX),
            ExceptionFor<ErrorCodes::StaleConfig>,
            validateException);

        phase.arrive_and_wait();
    });

    {
        phase.arrive_and_wait();
        // The timer only starts once the waiter calls signal->get(), so wait for it to register.
        while (getStatistics()["activeWaitersCount"].safeNumberLong() == 0) {
            sleepmillis(1);
        }
        // Now that the waiter is registered, sleep a known amount so the wait time is observable.
        sleepmillis(20);
        auto metrics = getStatistics();
        ASSERT_EQ(metrics["activeCatchupCount"].safeNumberLong(), 0);
        ASSERT_EQ(metrics["activeCommitCount"].safeNumberLong(), 1);
        ASSERT_GTE(metrics["totalTimeActiveCommitMillis"].safeNumberLong(), 20);
        ASSERT_EQ(metrics["activeWaitersCount"].safeNumberLong(), 1);
        ASSERT_GTE(metrics["totalTimeWaiting"].safeNumberLong(), 10);
    }

    {
        const auto& csr = CollectionShardingRuntime::acquireExclusive(operationContext(), kNss);
        csr->exitCriticalSection(operationContext(), criticalSectionReason);
    }

    phase.arrive_and_wait();

    auto metrics = getStatistics();
    ASSERT_EQ(metrics["activeCatchupCount"].safeNumberLong(), 0);
    ASSERT_EQ(metrics["activeCommitCount"].safeNumberLong(), 0);
    ASSERT_EQ(metrics["activeWaitersCount"].safeNumberLong(), 0);
    ASSERT_EQ(metrics["totalTimeWaiting"].safeNumberLong(), 0);

    waiter.join();
}

/**
 * Fixture for when range deletion functionality is required in CollectionShardingRuntime tests.
 */
class CollectionShardingRuntimeWithRangeDeleterTest : service_context_test::WithSetupTransportLayer,
                                                      public CollectionShardingRuntimeTest {
public:
    void setUp() override {
        CollectionShardingRuntimeTest::setUp();
        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());
        // Set up replication coordinator to be primary and have no replication delay.
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext());
        replCoord->setCanAcceptNonLocalWrites(true);
        std::ignore = replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY);
        // Make waitForWriteConcern return immediately.
        replCoord->setAwaitReplicationReturnValueFunction([this](OperationContext* opCtx,
                                                                 const repl::OpTime& opTime) {
            return repl::ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0));
        });
        repl::ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));

        createTestCollection(operationContext(), kTestNss);

        auto acq =
            acquireCollection(operationContext(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  operationContext(), kTestNss, AcquisitionPrerequisites::kWrite),
                              MODE_IX);
        _uuid = acq.uuid();

        auto opCtx = operationContext();
        RangeDeleterService::get(opCtx)->onStartup(opCtx);
        RangeDeleterService::get(opCtx)->onStepUpBegin(opCtx, 0L);
        RangeDeleterService::get(opCtx)->onStepUpComplete(opCtx, 0L);
        RangeDeleterService::get(opCtx)->getServiceUpFuture().get(opCtx);
    }

    void tearDown() override {
        DBDirectClient client(operationContext());
        client.dropCollection(kTestNss);

        RangeDeleterService::get(operationContext())->onStepDown();
        RangeDeleterService::get(operationContext())->onShutdown();
        WaitForMajorityService::get(getServiceContext()).shutDown();
        CollectionShardingRuntimeTest::tearDown();
    }

    CollectionShardingRuntime::ScopedExclusiveCollectionShardingRuntime csr() {
        return CollectionShardingRuntime::acquireExclusive(operationContext(), kTestNss);
    }

    const UUID& uuid() const {
        return _uuid;
    }

private:
    UUID _uuid{UUID::gen()};
};

// The range deleter service test util will register a task with the range deleter with pending set
// to true, insert the task, and then remove the pending field. We must create the task with pending
// set to true so that the removal of the pending field succeeds.
RangeDeletionTask createRangeDeletionTask(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const UUID& uuid,
                                          const ChunkRange& range,
                                          int64_t numOrphans) {
    auto migrationId = UUID::gen();
    RangeDeletionTask t(migrationId, nss, uuid, ShardId("donor"), range, CleanWhenEnum::kNow);
    t.setNumOrphanDocs(numOrphans);
    const auto currentTime = VectorClock::get(opCtx)->getTime();
    t.setTimestamp(currentTime.clusterTime().asTimestamp());
    t.setPending(true);
    return t;
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanReturnsErrorIfMetadataManagerDoesNotExist) {
    auto status = CollectionShardingRuntime::waitForClean(
        operationContext(),
        kTestNss,
        uuid(),
        ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)),
        Date_t::max());
    ASSERT_EQ(status.code(), ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanReturnsErrorIfCollectionUUIDDoesNotMatchFilteringMetadata) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx, uuid());
    csr()->setCollectionMetadata(opCtx, metadata);
    auto randomUuid = UUID::gen();

    auto status = CollectionShardingRuntime::waitForClean(
        opCtx,
        kTestNss,
        randomUuid,
        ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)),
        Date_t::max());
    ASSERT_EQ(status.code(), ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanReturnsOKIfNoDeletionsAreScheduled) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx, uuid());
    csr()->setCollectionMetadata(opCtx, metadata);

    auto status = CollectionShardingRuntime::waitForClean(
        opCtx,
        kTestNss,
        uuid(),
        ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)),
        Date_t::max());

    ASSERT_OK(status);
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanBlocksBehindOneScheduledDeletion) {
    // Enable fail point to suspendRangeDeletion.
    globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::alwaysOn);
    ScopeGuard resetFailPoint(
        [=] { globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::off); });

    OperationContext* opCtx = operationContext();

    auto metadata = makeShardedMetadata(opCtx, uuid());
    csr()->setCollectionMetadata(opCtx, metadata);
    const ChunkRange range = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY));

    const auto task = createRangeDeletionTask(opCtx, kTestNss, uuid(), range, 0);
    auto taskCompletionFuture = registerAndCreatePersistentTask(
        opCtx, task, SemiFuture<void>::makeReady() /* waitForActiveQueries */);

    opCtx->setDeadlineAfterNowBy(Milliseconds(100), ErrorCodes::MaxTimeMSExpired);
    auto status =
        CollectionShardingRuntime::waitForClean(opCtx, kTestNss, uuid(), range, Date_t::max());

    ASSERT_EQ(status.code(), ErrorCodes::MaxTimeMSExpired);

    globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::off);
    taskCompletionFuture.get();
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanBlocksBehindAllScheduledDeletions) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx, uuid());
    csr()->setCollectionMetadata(opCtx, metadata);

    const auto middleKey = 5;
    const ChunkRange range1 = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << middleKey));
    const auto task1 = createRangeDeletionTask(opCtx, kTestNss, uuid(), range1, 0);
    const ChunkRange range2 = ChunkRange(BSON(kShardKey << middleKey), BSON(kShardKey << MAXKEY));
    const auto task2 = createRangeDeletionTask(opCtx, kTestNss, uuid(), range2, 0);

    auto cleanupCompleteFirst = registerAndCreatePersistentTask(
        opCtx, task1, SemiFuture<void>::makeReady() /* waitForActiveQueries */);

    auto cleanupCompleteSecond = registerAndCreatePersistentTask(
        opCtx, task2, SemiFuture<void>::makeReady() /* waitForActiveQueries */);

    auto status = CollectionShardingRuntime::waitForClean(
        opCtx,
        kTestNss,
        uuid(),
        ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)),
        Date_t::max());

    // waitForClean should block until both cleanup tasks have run. This is a best-effort check,
    // since even if it did not block, it is possible that the cleanup tasks could complete before
    // reaching these lines.
    ASSERT(cleanupCompleteFirst.isReady());
    ASSERT(cleanupCompleteSecond.isReady());

    ASSERT_OK(status);
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanReturnsOKAfterSuccessfulDeletion) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx, uuid());
    csr()->setCollectionMetadata(opCtx, metadata);
    const ChunkRange range = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY));
    const auto task = createRangeDeletionTask(opCtx, kTestNss, uuid(), range, 0);

    auto cleanupComplete = registerAndCreatePersistentTask(
        opCtx, task, SemiFuture<void>::makeReady() /* waitForActiveQueries */);

    auto status =
        CollectionShardingRuntime::waitForClean(opCtx, kTestNss, uuid(), range, Date_t::max());

    ASSERT_OK(status);
    ASSERT(cleanupComplete.isReady());
}

class CollectionShardingRuntimeWithCatalogTest
    : public CollectionShardingRuntimeWithRangeDeleterTest {
public:
    void setUp() override {
        CollectionShardingRuntimeWithRangeDeleterTest::setUp();
    }

    void tearDown() override {
        OpObserver::Times::get(operationContext()).reservedOpTimes.clear();
        CollectionShardingRuntimeWithRangeDeleterTest::tearDown();
    }
};

// Test the CSR before and after the initialization of the ShardingState with ClusterRole::None.
TEST_F(ShardingMongoDTestFixture, ShardingStateDisabledReturnsUntrackedVersion) {
    OperationContext* opCtx = operationContext();
    const auto metadata = CollectionShardingRuntimeTest::makeShardedMetadata(opCtx);
    auto receivedShardVersion = ShardVersionFactory::make(metadata);
    ScopedSetShardRole scopedSetShardRole{
        opCtx, kTestNss, receivedShardVersion, boost::none /* databaseVersion */};

    // While the ShardingState has not yet been recovered, we expect the CollectionShardingRuntime
    // to present all collections as UNTRACKED.
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    ASSERT_DOES_NOT_THROW(csr.getCollectionDescription(opCtx));
    ASSERT_DOES_NOT_THROW(csr.checkShardVersionOrThrow(opCtx));

    // Setting the recovery completed as ClusterRole::None is also equilvament to initialize a
    // standalone replica-set. The CollectionShardingState should continue to present collections as
    // UNTRACKED.
    ShardingState::RecoveredClusterRole rcr;
    rcr.role = ClusterRole::None;
    auto shardingState = ShardingState::get(opCtx);
    shardingState->setRecoveryCompleted(rcr, UUID::gen());
    ASSERT_DOES_NOT_THROW(csr.getCollectionDescription(opCtx));
    ASSERT_DOES_NOT_THROW(csr.checkShardVersionOrThrow(opCtx));
}

// Test the CSR before and after the initialization of the ShardingState with shard server role.
TEST_F(ShardingMongoDTestFixture, ShardingStateEnabledReturnsTrackedVersion) {
    OperationContext* opCtx = operationContext();
    const auto metadata = CollectionShardingRuntimeTest::makeShardedMetadata(opCtx);
    auto receivedShardVersion = ShardVersionFactory::make(metadata);
    ScopedSetShardRole scopedSetShardRole{
        opCtx, kTestNss, receivedShardVersion, boost::none /* databaseVersion */};

    // While the ShardingState has not yet been recovered, we expect the CollectionShardingRuntime
    // to present all collections as UNTRACKED.
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    ASSERT_DOES_NOT_THROW(csr.getCollectionDescription(opCtx));
    ASSERT_DOES_NOT_THROW(csr.checkShardVersionOrThrow(opCtx));

    // After completing the ShardingState recovery as a ClusterRole::ShardServer,
    // CollectionShardingRuntime will throw StaleConfig because the metadata needs to be recovered.
    ShardingState::RecoveredClusterRole rcr;
    rcr.role = ClusterRole::ShardServer;
    rcr.shardId = ShardId("0");
    ShardingState::get(opCtx)->setRecoveryCompleted(rcr, UUID::gen());
    ASSERT_THROWS_CODE(csr.getCollectionDescription(opCtx), DBException, ErrorCodes::StaleConfig);
    ASSERT_THROWS_CODE(csr.checkShardVersionOrThrow(opCtx), DBException, ErrorCodes::StaleConfig);
}

TEST_F(CollectionShardingRuntimeTest, OnInvalidateCollectionMetadataClearsCSRWithMatchingUUID) {
    auto opCtx = operationContext();
    createTestCollection(opCtx, kTestNss);
    auto collUuid = UUID::gen();
    {
        auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
        scopedCsr->setCollectionMetadata(opCtx,
                                         makeShardedMetadata(opCtx, collUuid),
                                         CollectionShardingRuntime::NoRoutingTableAs::kUntracked);
        scopedCsr->setAuthoritative();
    }

    {
        auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
        ASSERT_TRUE(csr->getCurrentMetadataIfKnown());
    }

    auto oplogEntry = makeInvalidateCollectionMetadataOplogEntry(kTestNss, collUuid);
    ShardServerOpObserver observer;
    observer.onInvalidateCollectionMetadata(opCtx, oplogEntry);

    {
        auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
        ASSERT_FALSE(csr->getCurrentMetadataIfKnown());
        ASSERT_EQ(csr->getAuthoritativeState(),
                  CollectionShardingRuntime::AuthoritativeState::kAuthoritative);
    }
}

TEST_F(CollectionShardingRuntimeTest, OnApplyCollectionShardingStateDeltaCSRWithMatchingUUID) {
    auto opCtx = operationContext();
    createTestCollection(opCtx, kTestNss);
    auto collUuid = UUID::gen();
    auto originalMetadata = makeShardedMetadata(opCtx, collUuid);
    CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss)
        ->setCollectionMetadata(
            opCtx, originalMetadata, CollectionShardingRuntime::NoRoutingTableAs::kUntracked);

    auto newVersion = originalMetadata.getCollPlacementVersion();
    newVersion.incMajor();
    ChunkType changedChunk(collUuid,
                           ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)),
                           newVersion,
                           ShardId("other"));
    changedChunk.setName(OID::gen());

    auto oplogEntry =
        makeCollectionShardingStateDeltaOplogEntry(kTestNss, collUuid, {changedChunk});
    ShardServerOpObserver observer;
    observer.onApplyCollectionShardingStateDelta(opCtx, oplogEntry);

    {
        auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
        auto metadata = csr->getCurrentMetadataIfKnown();
        ASSERT_TRUE(metadata);
        ASSERT_EQ(metadata->getCollPlacementVersion(), newVersion);
        ASSERT_EQ(csr->getAuthoritativeState(),
                  CollectionShardingRuntime::AuthoritativeState::kAuthoritative);
    }
}

TEST_F(CollectionShardingRuntimeTest, OnApplyCollectionShardingStateDeltaWithoutKnownMetadata) {
    auto opCtx = operationContext();
    createTestCollection(opCtx, kTestNss);
    auto collUuid = UUID::gen();

    // Creating the collection installs untracked filtering metadata. Clear it so the CSR has no
    // known metadata, leaving no base for the delta to apply to.
    CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss)->clearCollectionMetadata(opCtx);
    {
        auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
        ASSERT_FALSE(csr->getCurrentMetadataIfKnown());
    }

    ChunkType changedChunk(collUuid,
                           ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)),
                           ChunkVersion({OID::gen(), Timestamp(Date_t::now())}, {1, 0}),
                           ShardId("other"));
    changedChunk.setName(OID::gen());

    auto oplogEntry =
        makeCollectionShardingStateDeltaOplogEntry(kTestNss, collUuid, {changedChunk});
    ShardServerOpObserver observer;
    observer.onApplyCollectionShardingStateDelta(opCtx, oplogEntry);

    // The delta is dropped and the collection stays in an authoritative-but-unknown state, forcing
    // the next user to perform a full recovery from disk.
    {
        auto csr = CollectionShardingRuntime::acquireShared(opCtx, kTestNss);
        ASSERT_FALSE(csr->getCurrentMetadataIfKnown());
        ASSERT_EQ(csr->getAuthoritativeState(),
                  CollectionShardingRuntime::AuthoritativeState::kAuthoritative);
    }
}

DEATH_TEST_REGEX_F(CollectionShardingRuntimeTestDeathTest,
                   OnApplyCollectionShardingStateDeltaWithMismatchedUUID,
                   "Tripwire assertion.*12698707") {
    auto opCtx = operationContext();
    createTestCollection(opCtx, kTestNss);
    auto collUuid = UUID::gen();
    auto originalMetadata = makeShardedMetadata(opCtx, collUuid);
    CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss)
        ->setCollectionMetadata(
            opCtx, originalMetadata, CollectionShardingRuntime::NoRoutingTableAs::kUntracked);

    // The delta carries a collection UUID that does not match the installed metadata.
    auto otherUuid = UUID::gen();
    auto newVersion = originalMetadata.getCollPlacementVersion();
    newVersion.incMajor();
    ChunkType changedChunk(otherUuid,
                           ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)),
                           newVersion,
                           ShardId("other"));
    changedChunk.setName(OID::gen());

    auto oplogEntry =
        makeCollectionShardingStateDeltaOplogEntry(kTestNss, otherUuid, {changedChunk});
    ShardServerOpObserver observer;
    observer.onApplyCollectionShardingStateDelta(opCtx, oplogEntry);
}

}  // namespace mongo
