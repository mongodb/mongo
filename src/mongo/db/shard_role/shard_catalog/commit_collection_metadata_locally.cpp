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
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
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
                                        const CollectionType& coll,
                                        const boost::optional<int>& limit = boost::none) {
    auto shardId = ShardingState::get(opCtx)->shardId();
    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto chunksStatus = catalogClient->getChunks(
        opCtx,
        BSON(ChunkType::collectionUUID()
             << coll.getUuid() << "$or"
             << BSON_ARRAY(BSON(ChunkType::shard(shardId.toString()))
                           << BSON("history.shard" << shardId.toString()))),
        BSON(ChunkType::min() << 1) /* sort */,
        limit,
        nullptr,
        coll.getEpoch(),
        coll.getTimestamp(),
        repl::ReadConcernLevel::kSnapshotReadConcern);
    uassertStatusOK(chunksStatus.getStatus());

    return chunksStatus.getValue();
}

bool doesShardOwnChunks(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const CollectionType& coll) {
    const auto chunkList = fetchOwnedChunks(opCtx, nss, coll, 1);
    return !chunkList.empty();
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

void deleteCollectionEntryLocally(OperationContext* opCtx, const NamespaceString& nss) {
    DBDirectClient dbClient(opCtx);
    auto serializedNs = NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());

    executeLocalDelete(dbClient,
                       NamespaceString::kConfigShardCatalogCollectionsNamespace,
                       BSON(CollectionType::kNssFieldName << serializedNs),
                       false /* multi */);
}

void upsertCollectionEntryLocally(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const CollectionType& coll) {
    DBDirectClient dbClient{opCtx};

    auto entry =
        makeUpsertEntry(BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                                 nss, SerializationContext::stateDefault())),
                        coll.asShardCatalogType().toBSON());

    executeLocalUpdates(
        dbClient, NamespaceString::kConfigShardCatalogCollectionsNamespace, {entry});
}

void deleteCollectionChunksMetadataLocally(OperationContext* opCtx, const UUID& uuid) {
    DBDirectClient dbClient(opCtx);
    executeLocalDelete(dbClient,
                       NamespaceString::kConfigShardCatalogChunksNamespace,
                       BSON(ChunkType::collectionUUID() << uuid),
                       true /* multi */);
}

void deleteAllCollectionMetadataLocally(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const UUID& uuid) {
    deleteCollectionEntryLocally(opCtx, nss);
    deleteCollectionChunksMetadataLocally(opCtx, uuid);
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

    scopedCsr->setFilteringMetadata_authoritative(
        opCtx, std::move(ownedMetadata), CollectionShardingRuntime::NoRoutingTableAs::kUntracked);
}

void clearShardCatalogCacheForDroppedCollection(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const UUID& uuid) {
    auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, nss);

    scopedCsr->clearFilteringMetadataForDroppedCollection_authoritative(opCtx, uuid);
}

}  // namespace

void commitDropCollectionLocally(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const UUID& uuid) {

    LOGV2_INFO(12295703,
               "Dropping all shard catalog metadata for collection",
               "nss"_attr = nss,
               "uuid"_attr = uuid);

    // Write to `config.shard.catalog.(collections|chunks)` to delete collection metadata.
    deleteAllCollectionMetadataLocally(opCtx, nss, uuid);

    // Write an oplog 'c' entry to invalidate secondaries CSR.
    invalidateCollectionMetadataOnSecondaries(opCtx, nss, uuid, true /* forDroppedCollection */);

    // Clear this node collection metadata from CSR.
    clearShardCatalogCacheForDroppedCollection(opCtx, nss, uuid);
}

void commitDropOfStaleChunksForRename(OperationContext* opCtx, const UUID& uuid) {
    // Delete the old chunks from `config.shard.catalog.chunks`. The deletion/replacement of the
    // collection entry happened before as part of calling commitRenameOfCollectionMetadata.
    deleteCollectionChunksMetadataLocally(opCtx, uuid);
}

void commitRenameOfCollectionMetadata(OperationContext* opCtx,
                                      const NamespaceString& fromNss,
                                      const boost::optional<UUID>& fromUUID,
                                      const NamespaceString& toNss,
                                      const boost::optional<UUID>& targetUUID,
                                      const boost::optional<UUID>& newTargetUUID,
                                      bool isUpgrading,
                                      bool isDbPrimary) {
    // Delete the old collection entries if any exist since they'll be replaced by the final
    // state.
    deleteCollectionEntryLocally(opCtx, fromNss);
    deleteCollectionEntryLocally(opCtx, toNss);

    auto cleanupInMemoryState = [&](bool clearFromNss, bool clearToNss) {
        // At this point we've fully modified the durable state to reflect the final state. We have
        // to clear out the in-memory state if the collection got replaced.
        if (clearFromNss && fromUUID) {
            // Write an oplog 'c' entry to invalidate collection metadata on secondaries.
            invalidateCollectionMetadataOnSecondaries(
                opCtx, fromNss, *fromUUID, true /* forDroppedCollection */);

            clearShardCatalogCacheForDroppedCollection(opCtx, fromNss, *fromUUID);
        }

        if (clearToNss && targetUUID) {
            // Write an oplog 'c' entry to invalidate collection metadata on secondaries.
            invalidateCollectionMetadataOnSecondaries(
                opCtx, toNss, *targetUUID, true /* forDroppedCollection */);

            clearShardCatalogCacheForDroppedCollection(opCtx, toNss, *targetUUID);
        }
    };

    if (isUpgrading || newTargetUUID.has_value()) {
        // If it's upgrading then we just need to fetch the entire filtering metadata from the CSRS
        // and durably persist it since the local state may be incomplete. This means that we have
        // to first drop the old collection metadata and subsequently fetch the new one, replacing
        // the existing entries if necessary.
        //
        // We also perform this if the collection has undergone a UUID change since that invalidates
        // all previous chunks as well and the new chunks aren't present in the shard yet.
        LOGV2_INFO(12295702,
                   "Renaming metadata during an FCV upgrade, forcing a full refresh of the data on "
                   "the shard",
                   "fromNss"_attr = fromNss,
                   "toNss"_attr = toNss);

        if (fromUUID) {
            commitDropCollectionLocally(opCtx, fromNss, *fromUUID);
        }
        try {
            auto _ = fetchCollection(opCtx, toNss);
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
            LOGV2_INFO(12295701,
                       "Nothing to persist in the local shard catalog since the renamed collection "
                       "is untracked",
                       "toNss"_attr = toNss,
                       "error"_attr = ex.toStatus());
            cleanupInMemoryState(false, true);
            return;
        }
        commitCollectionMetadataLocally(opCtx, toNss, isDbPrimary);
        return;
    }

    // At this point we can assume that the shard catalog metadata is durably persisted on this
    // shard, so all the chunk entries are stored locally for both from/target collections if they
    // are owned by this shard.
    //
    // There are two paths to take here:
    // 1. Either the new collection state is tracked (there's a collection entry on the CSRS)
    // 2. Or it's untracked (No collection entry on the CSRS)
    try {
        // For the first case we must delete the collection entries for both fromNss and toNss
        // namespaces and then insert the final entry for toNss. We've already deleted the first
        // collection entry so we must upsert the new entry to end up in the final form.
        auto newEntry = fetchCollection(opCtx, toNss);
        auto ownsChunks = doesShardOwnChunks(opCtx, toNss, newEntry);

        if (ownsChunks || isDbPrimary) {
            upsertCollectionEntryLocally(opCtx, toNss, newEntry);
        }
        // The old chunks will now get cleaned up outside of the critical section if the rename
        // actually replaced an existing sharded collection.
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
        // For the second one we have to delete all entries which has already occurred at the
        // beginning.
        LOGV2_INFO(12295700,
                   "Target collection is untracked, nothing to persist in the local shard catalog",
                   "toNss"_attr = toNss,
                   "error"_attr = ex);
    }
    cleanupInMemoryState(true /*clearFromNss*/, true /*clearToNss*/);

    // TODO SERVER-127215: It should be possible to recover the filtering metadata at this point for
    // the collection. However, we've chosen not to do it and defer that to the first query in order
    // to simplify the rename path.
}

void commitCollectionMetadataLocally(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     bool isDbPrimaryShard) {
    auto coll = fetchCollection(opCtx, nss);
    auto ownedChunks = fetchOwnedChunks(opCtx, nss, coll);

    // Drop any prior chunk entries for this collection so repeated calls (e.g. when an unsplittable
    // collection is sharded) don't accumulate stale rows. The new chunk documents may carry
    // different OIDs, in which case writeCollectionMetadataLocally's upsert-by-OID would insert
    // rather than replace.
    deleteCollectionChunksMetadataLocally(opCtx, coll.getUuid());

    if (isDbPrimaryShard || !ownedChunks.empty()) {
        // Write to `config.shard.catalog.(collections|chunks)` to insert collection metadata.
        writeCollectionMetadataLocally(opCtx, nss, coll.asShardCatalogType(), ownedChunks);

        // Update this node CSR with collection metadata and chunks.
        updateShardCatalogCache(opCtx, nss, coll, ownedChunks);
    } else {
        // If the shard owns no chunks AND is not the dbPrimary then it really doesn't know anything
        // about the collection. Make sure to delete any existing collection entry as well and clear
        // the CSR.
        deleteCollectionEntryLocally(opCtx, nss);
        clearShardCatalogCacheForDroppedCollection(opCtx, nss, coll.getUuid());
    }

    // Write an oplog 'c' entry to invalidate collection metadata on secondaries.
    invalidateCollectionMetadataOnSecondaries(
        opCtx, nss, coll.getUuid(), false /* forDroppedCollection */);
}

}  // namespace shard_catalog_commit
}  // namespace mongo
