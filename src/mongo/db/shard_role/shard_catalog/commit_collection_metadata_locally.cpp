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

#include "mongo/db/shard_role/shard_catalog/commit_collection_metadata_locally.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/type_oplog_catalog_metadata_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace shard_catalog_commit {
namespace {

CollectionType fetchCollection(OperationContext* opCtx, const NamespaceString& nss) {
    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto coll = catalogClient->getCollection(opCtx, nss);
    return coll;
}

std::vector<ChunkType> fetchOwnedChunks(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const CollectionType& coll) {
    // TODO (SERVER-121707): Fetch only owned chunks or owned in the past (history).

    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto chunksStatus =
        catalogClient->getChunks(opCtx,
                                 BSON(ChunkType::collectionUUID() << coll.getUuid()),
                                 BSON(ChunkType::min() << 1) /* sort */,
                                 boost::none,
                                 nullptr,
                                 coll.getEpoch(),
                                 coll.getTimestamp(),
                                 repl::ReadConcernLevel::kSnapshotReadConcern);
    uassertStatusOK(chunksStatus.getStatus());

    return chunksStatus.getValue();
}

void writeCollectionMetadataLocally(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const CollectionType& coll,
                                    const std::vector<ChunkType>& chunks) {
    DBDirectClient dbClient(opCtx);

    // Persist Collection Metadata
    write_ops::UpdateCommandRequest collUpdateReq(
        NamespaceString::kConfigShardCatalogCollectionsNamespace);
    {
        write_ops::UpdateOpEntry entry;
        const auto serializedNs =
            NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());
        entry.setQ(BSON(CollectionType::kNssFieldName << serializedNs));
        entry.setU(coll.toBSON());
        entry.setUpsert(true);
        entry.setMulti(false);
        collUpdateReq.setUpdates({std::move(entry)});
    }

    collUpdateReq.setWriteConcern(defaultMajorityWriteConcern());
    write_ops::checkWriteErrors(dbClient.update(collUpdateReq));

    tassert(
        10281500, "Expected to find at least one chunk for a tracked collection", !chunks.empty());

    // TODO (SERVER-121708): Investigate doing the following writes when they exceed 16MB.

    // Persist Chunk Metadata
    write_ops::UpdateCommandRequest chunkUpdateReq(
        NamespaceString::kConfigShardCatalogChunksNamespace);
    std::vector<write_ops::UpdateOpEntry> chunkUpdates;
    chunkUpdates.reserve(chunks.size());

    for (const auto& chunk : chunks) {
        write_ops::UpdateOpEntry entry;
        entry.setQ(BSON(ChunkType::name() << chunk.getName()));
        entry.setU(chunk.toConfigBSON());
        entry.setUpsert(true);
        entry.setMulti(false);
        chunkUpdates.push_back(std::move(entry));
    }
    chunkUpdateReq.setUpdates(std::move(chunkUpdates));
    chunkUpdateReq.setWriteConcern(defaultMajorityWriteConcern());
    write_ops::checkWriteErrors(dbClient.update(chunkUpdateReq));
}

void deleteChunksMetadataLocally(OperationContext* opCtx, const BSONObj matchingQuery) {
    DBDirectClient dbClient(opCtx);

    write_ops::DeleteCommandRequest deleteOp(NamespaceString::kConfigShardCatalogChunksNamespace);
    deleteOp.setDeletes({[&] {
        write_ops::DeleteOpEntry entry;
        entry.setQ(matchingQuery);
        entry.setMulti(true);
        return entry;
    }()});
    deleteOp.setWriteConcern(defaultMajorityWriteConcern());
    write_ops::checkWriteErrors(dbClient.remove(std::move(deleteOp)));
}

void invalidateCollectionMetadataOnSecondaries(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               const UUID& uuid) {
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setVersionContextIfHasOperationFCV(VersionContext::getDecoration(opCtx));
    oplogEntry.setNss(nss);
    oplogEntry.setUuid(uuid);
    oplogEntry.setObject(InvalidateCollectionMetadataOplogEntry{std::string(nss.coll())}.toBSON());
    oplogEntry.setOpTime(OplogSlot());
    oplogEntry.setWallClockTime(opCtx->fastClockSource().now());

    writeConflictRetry(
        opCtx, "invalidateCollectionMetadata", NamespaceString::kRsOplogNamespace, [&] {
            AutoGetOplogFastPath oplogWrite(opCtx, OplogAccessMode::kWrite);
            WriteUnitOfWork wuow(opCtx);
            repl::OpTime opTime = repl::logOp(opCtx, &oplogEntry);
            uassert(10281501,
                    str::stream() << "Failed to create new oplog entry for "
                                     "invalidateCollectionMetadata with opTime: "
                                  << oplogEntry.getOpTime().toString() << ": "
                                  << redact(oplogEntry.toBSON()),
                    !opTime.isNull());
            wuow.commit();
        });
}

void updateShardCatalogCache(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const CollectionType& coll,
                             const std::vector<ChunkType>& chunks) {
    // TODO (SERVER-121707): Make collection metadata support unowned gaps.

    auto thisShardId = ShardingState::get(opCtx)->shardId();
    auto rt = [&] {
        auto defaultCollator = [&]() -> std::unique_ptr<CollatorInterface> {
            if (!coll.getDefaultCollation().isEmpty()) {
                return uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                           ->makeFromBSON(coll.getDefaultCollation()));
            }
            return nullptr;
        }();

        return RoutingTableHistory::makeNew(nss,
                                            coll.getUuid(),
                                            coll.getKeyPattern(),
                                            coll.getUnsplittable().value_or(false),
                                            std::move(defaultCollator),
                                            coll.getUnique(),
                                            coll.getEpoch(),
                                            coll.getTimestamp(),
                                            coll.getTimeseriesFields(),
                                            coll.getReshardingFields(),
                                            coll.getAllowMigrations(),
                                            chunks);
    }();

    auto version = rt.getVersion();
    auto rtHandle =
        RoutingTableHistoryValueHandle(std::make_shared<RoutingTableHistory>(std::move(rt)),
                                       ComparableChunkVersion::makeComparableChunkVersion(version));

    CollectionMetadata ownedMetadata(CurrentChunkManager(std::move(rtHandle)), thisShardId);

    auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, nss);
    scopedCsr->setFilteringMetadata_authoritative(opCtx, std::move(ownedMetadata));
}
}  // namespace

void commitRefineShardKeyLocally(OperationContext* opCtx, const NamespaceString& nss) {
    auto coll = fetchCollection(opCtx, nss);
    auto ownedChunks = fetchOwnedChunks(opCtx, nss, coll);

    // Write to `config.shard.catalog.(collections|chunks)` to insert collection metadata.
    writeCollectionMetadataLocally(opCtx, nss, coll, ownedChunks);

    // Delete stale chunks from config.shard.catalog.chunks whose shard key bounds do not match the
    // refined key pattern. This can occur when the shard catalog has an out-of-date view of the
    // owned chunk ranges (e.g., due to splits or merges).
    // TODO (SERVER-121709): Evaluate if this holds once merge/split are authoritative.
    const int numKeyFields = coll.getKeyPattern().toBSON().nFields();
    BSONObj query =
        BSON(ChunkType::collectionUUID()
             << coll.getUuid() << "$expr"
             << BSON("$ne" << BSON_ARRAY(BSON("$size" << BSON("$objectToArray" << "$min"))
                                         << numKeyFields)));
    deleteChunksMetadataLocally(opCtx, std::move(query));

    // Write an oplog 'c' entry to invalidate collection metadata on secondaries.
    invalidateCollectionMetadataOnSecondaries(opCtx, nss, coll.getUuid());

    // Update this node CSR with collection metadata and chunks.
    updateShardCatalogCache(opCtx, nss, coll, ownedChunks);
}

}  // namespace shard_catalog_commit
}  // namespace mongo
