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
    auto shardId = ShardingState::get(opCtx)->shardId();
    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto chunksStatus = catalogClient->getChunks(
        opCtx,
        BSON(ChunkType::collectionUUID()
             << coll.getUuid() << "$or"
             << BSON_ARRAY(BSON(ChunkType::shard(shardId.toString()))
                           << BSON("history.shard" << shardId.toString()))),
        BSON(ChunkType::min() << 1) /* sort */,
        boost::none,
        nullptr,
        coll.getEpoch(),
        coll.getTimestamp(),
        repl::ReadConcernLevel::kSnapshotReadConcern);
    uassertStatusOK(chunksStatus.getStatus());

    return chunksStatus.getValue();
}

write_ops::UpdateOpEntry makeUpsertEntry(BSONObj filter, BSONObj replacement) {
    write_ops::UpdateOpEntry entry;
    entry.setQ(std::move(filter));
    entry.setU(std::move(replacement));
    entry.setUpsert(true);
    entry.setMulti(false);
    return entry;
}

void executeLocalUpdates(DBDirectClient& dbClient,
                         const NamespaceString& nss,
                         std::vector<write_ops::UpdateOpEntry> updates) {
    write_ops::UpdateCommandRequest req(nss);
    req.setUpdates(std::move(updates));
    req.setWriteConcern(defaultMajorityWriteConcern());
    write_ops::checkWriteErrors(dbClient.update(req));
}

void executeLocalDelete(DBDirectClient& dbClient,
                        const NamespaceString& nss,
                        BSONObj query,
                        bool multi) {
    write_ops::DeleteCommandRequest req(nss);
    write_ops::DeleteOpEntry entry;
    entry.setQ(std::move(query));
    entry.setMulti(multi);
    req.setDeletes({std::move(entry)});
    req.setWriteConcern(defaultMajorityWriteConcern());
    write_ops::checkWriteErrors(dbClient.remove(std::move(req)));
}

void writeCollectionMetadataLocally(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const ShardCatalogCollectionTypeBase& coll,
                                    const std::vector<ChunkType>& chunks) {
    DBDirectClient dbClient(opCtx);

    auto serializedNs = NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());
    executeLocalUpdates(
        dbClient,
        NamespaceString::kConfigShardCatalogCollectionsNamespace,
        {makeUpsertEntry(BSON(ShardCatalogCollectionTypeBase::kNssFieldName << serializedNs),
                         coll.toBSON())});

    tassert(
        10281500, "Expected to find at least one chunk for a tracked collection", !chunks.empty());

    // Persist Chunk Metadata, we do this in batches because of the 16MB BSON limit.
    for (auto it = chunks.begin(); it != chunks.end();) {
        size_t updateSize = 0;
        std::vector<write_ops::UpdateOpEntry> chunkUpdates;

        while (it != chunks.end()) {
            const auto& chunk = *it;

            auto q = BSON(ChunkType::name() << chunk.getName());
            updateSize += q.objsize();
            auto u = chunk.toConfigBSON();
            updateSize += u.objsize();

            write_ops::UpdateOpEntry entry;
            entry.setQ(std::move(q));
            entry.setU(std::move(u));
            entry.setUpsert(true);
            entry.setMulti(false);
            chunkUpdates.push_back(std::move(entry));

            it++;

            // In principle we could do a better approximation of the update request size in order
            // to be closer to the 16MB limit. However, after a certain batch size the speedup
            // improvement reaches a limit as can be seen with batched deletes. 10MB is a reasonable
            // limit that will accommodate all batched update requests comfortably under the 16MB
            // limit while offering the same advantages.
            static constexpr auto k10MbLimit = 10 * 1024 * 1024;
            if (updateSize >= k10MbLimit || chunkUpdates.size() >= write_ops::kMaxWriteBatchSize) {
                break;
            }
        }

        executeLocalUpdates(
            dbClient, NamespaceString::kConfigShardCatalogChunksNamespace, std::move(chunkUpdates));
    }
}

void deleteCollectionMetadataLocally(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const UUID& uuid) {
    DBDirectClient dbClient(opCtx);
    auto serializedNs = NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());

    executeLocalDelete(dbClient,
                       NamespaceString::kConfigShardCatalogCollectionsNamespace,
                       BSON(CollectionType::kNssFieldName << serializedNs),
                       false /* multi */);

    executeLocalDelete(dbClient,
                       NamespaceString::kConfigShardCatalogChunksNamespace,
                       BSON(ChunkType::collectionUUID() << uuid),
                       true /* multi */);
}

void invalidateCollectionMetadataOnSecondaries(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               const UUID& uuid,
                                               bool forDroppedCollection) {
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setVersionContextIfHasOperationFCV(VersionContext::getDecoration(opCtx));
    oplogEntry.setNss(nss);
    oplogEntry.setUuid(uuid);
    auto entry = InvalidateCollectionMetadataOplogEntry{std::string(nss.coll())};
    entry.setForDroppedCollection(forDroppedCollection);
    oplogEntry.setObject(entry.toBSON());
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
    auto thisShardId = ShardingState::get(opCtx)->shardId();
    auto rt = [&] {
        auto defaultCollator = [&]() -> std::unique_ptr<CollatorInterface> {
            if (!coll.getDefaultCollation().isEmpty()) {
                return uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                           ->makeFromBSON(coll.getDefaultCollation()));
            }
            return nullptr;
        }();

        return RoutingTableHistory::makeNewAllowingGaps(nss,
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

    // TODO (SERVER-123844): Switch to authoritative set once the untracked version doesn't need
    // to wait for configTime.
    scopedCsr->setFilteringMetadata_nonAuthoritative(opCtx, std::move(ownedMetadata));
}

void clearShardCatalogCacheForDroppedCollection(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const UUID& uuid) {
    auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, nss);

    // TODO (SERVER-123844): Switch to authoritative clear once the untracked version doesn't need
    // to wait for configTime.
    scopedCsr->clearFilteringMetadataForDroppedCollection_nonAuthoritative(opCtx);
}

}  // namespace

void commitRefineShardKeyLocally(OperationContext* opCtx, const NamespaceString& nss) {
    auto coll = fetchCollection(opCtx, nss);
    auto ownedChunks = fetchOwnedChunks(opCtx, nss, coll);

    // Write to `config.shard.catalog.(collections|chunks)` to insert collection metadata.
    writeCollectionMetadataLocally(opCtx, nss, coll.asShardCatalogType(), ownedChunks);

    // Delete stale chunks from config.shard.catalog.chunks whose shard key bounds do not match the
    // refined key pattern. This can occur when the shard catalog has an out-of-date view of the
    // owned chunk ranges (e.g., due to splits or merges).
    // TODO (SERVER-121709): Evaluate if this holds once merge/split are authoritative.
    const int numKeyFields = coll.getKeyPattern().toBSON().nFields();
    {
        DBDirectClient dbClient(opCtx);
        executeLocalDelete(
            dbClient,
            NamespaceString::kConfigShardCatalogChunksNamespace,
            BSON(ChunkType::collectionUUID()
                 << coll.getUuid() << "$expr"
                 << BSON("$ne" << BSON_ARRAY(BSON("$size" << BSON("$objectToArray" << "$min"))
                                             << numKeyFields))),
            true /* multi */);
    }

    // Write an oplog 'c' entry to invalidate collection metadata on secondaries.
    invalidateCollectionMetadataOnSecondaries(
        opCtx, nss, coll.getUuid(), false /* forDroppedCollection */);

    // Update this node CSR with collection metadata and chunks.
    updateShardCatalogCache(opCtx, nss, coll, ownedChunks);
}

void commitDropCollectionLocally(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const UUID& uuid) {
    // Write to `config.shard.catalog.(collections|chunks)` to delete collection metadata.
    deleteCollectionMetadataLocally(opCtx, nss, uuid);

    // Write an oplog 'c' entry to invalidate secondaries CSR.
    invalidateCollectionMetadataOnSecondaries(opCtx, nss, uuid, true /* forDroppedCollection */);

    // Clear this node collection metadata from CSR.
    clearShardCatalogCacheForDroppedCollection(opCtx, nss, uuid);
}

void commitCreateCollectionLocally(OperationContext* opCtx, const NamespaceString& nss) {
    auto coll = fetchCollection(opCtx, nss);
    auto ownedChunks = fetchOwnedChunks(opCtx, nss, coll);

    // Write to `config.shard.catalog.(collections|chunks)` to insert collection metadata.
    writeCollectionMetadataLocally(opCtx, nss, coll.asShardCatalogType(), ownedChunks);

    // Write an oplog 'c' entry to invalidate collection metadata on secondaries.
    invalidateCollectionMetadataOnSecondaries(
        opCtx, nss, coll.getUuid(), false /* forDroppedCollection */);

    // Update this node CSR with collection metadata and chunks.
    updateShardCatalogCache(opCtx, nss, coll, ownedChunks);
}

void commitCreateCollectionChunklessLocally(OperationContext* opCtx, const NamespaceString& nss) {
    auto coll = fetchCollection(opCtx, nss);

    // This shard does not own any chunks, but we still need the CSS to know the collection is
    // tracked. Persist a single placeholder chunk so that disk recovery can distinguish a
    // chunkless-tracked collection from an untracked one without special-case logic.
    auto range = ChunkRange(coll.getKeyPattern().globalMin(), coll.getKeyPattern().globalMax());
    ChunkType placeholder(coll.getUuid(),
                          std::move(range),
                          ChunkVersion({coll.getEpoch(), coll.getTimestamp()}, {1, 0}),
                          kChunklessPlaceholderShardId);
    placeholder.setName(OID::gen());
    std::vector<ChunkType> placeholderChunks{std::move(placeholder)};

    // Write the collection document and the placeholder chunk to the shard catalog.
    writeCollectionMetadataLocally(opCtx, nss, coll.asShardCatalogType(), placeholderChunks);

    // Write an oplog 'c' entry to invalidate collection metadata on secondaries.
    invalidateCollectionMetadataOnSecondaries(
        opCtx, nss, coll.getUuid(), false /* forDroppedCollection */);

    // Update this node CSR with chunkless tracked metadata.
    updateShardCatalogCache(opCtx, nss, coll, placeholderChunks);
}

}  // namespace shard_catalog_commit
}  // namespace mongo
