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
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/type_oplog_catalog_metadata_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
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

bool doesShardOwnChunks(OperationContext* opCtx, const NamespaceString& nss) {
    const auto coll = fetchCollection(opCtx, nss);
    return doesShardOwnChunks(opCtx, nss, coll);
}

write_ops::UpdateOpEntry makeUpdateEntry(BSONObj filter, BSONObj replacement) {
    write_ops::UpdateOpEntry entry;
    entry.setQ(std::move(filter));
    entry.setU(std::move(replacement));
    entry.setUpsert(false);
    entry.setMulti(false);
    return entry;
}

write_ops::UpdateOpEntry makeUpsertEntry(BSONObj filter, BSONObj replacement) {
    write_ops::UpdateOpEntry entry = makeUpdateEntry(std::move(filter), std::move(replacement));
    entry.setUpsert(true);
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

void writeCollectionAllowChunkOperationsLocally(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                bool allowChunkOperations) {
    const auto serializedNs =
        NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());

    DBDirectClient dbClient(opCtx);

    executeLocalUpdates(
        dbClient,
        NamespaceString::kConfigShardCatalogCollectionsNamespace,
        {makeUpdateEntry(
            BSON(CollectionType::kNssFieldName << serializedNs),
            allowChunkOperations
                ? BSON("$unset" << BSON(CollectionType::kAllowChunkOperationsFieldName << ""))
                : BSON("$set" << BSON(CollectionType::kAllowChunkOperationsFieldName << false)))});
}

void writeCollectionMetadataLocally(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const ShardCatalogCollectionTypeBase& coll,
                                    const std::vector<ChunkType>& chunks) {
    DBDirectClient dbClient(opCtx);

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

    auto serializedNs = NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());
    executeLocalUpdates(
        dbClient,
        NamespaceString::kConfigShardCatalogCollectionsNamespace,
        {makeUpsertEntry(BSON(ShardCatalogCollectionTypeBase::kNssFieldName << serializedNs),
                         coll.toBSON())});
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

void invalidateCollectionMetadata(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const UUID& uuid,
                                  bool forDroppedCollection,
                                  bool authoritative = true) {
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setVersionContextIfHasOperationFCV(VersionContext::getDecoration(opCtx));
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(uuid);
    auto entry = InvalidateCollectionMetadataOplogEntry{std::string(nss.coll())};
    entry.setForDroppedCollection(forDroppedCollection);
    entry.setNonAuth(!authoritative);
    oplogEntry.setObject(entry.toBSON());
    oplogEntry.setOpTime(OplogSlot());
    oplogEntry.setWallClockTime(opCtx->fastClockSource().now());

    // Replicate the invalidation through the oplog to secondaries via a 'c' entry.
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

    // Apply the invalidation on the current (primary) node after the timestamp has been assigned.
    opCtx->getServiceContext()->getOpObserver()->onInvalidateCollectionMetadata(
        opCtx, repl::OplogEntry(oplogEntry.toBSON()));
}

void setAllowChunkOperationsOnSecondaries(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const UUID& uuid,
                                          bool allowChunkOperations) {
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setVersionContextIfHasOperationFCV(VersionContext::getDecoration(opCtx));
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(uuid);
    auto entry = SetAllowChunkOperationsOplogEntry{std::string(nss.coll()), allowChunkOperations};
    oplogEntry.setObject(entry.toBSON());
    oplogEntry.setOpTime(OplogSlot());
    oplogEntry.setWallClockTime(opCtx->fastClockSource().now());

    writeConflictRetry(opCtx, "setAllowChunkOperations", NamespaceString::kRsOplogNamespace, [&] {
        AutoGetOplogFastPath oplogWrite(opCtx, OplogAccessMode::kWrite);
        WriteUnitOfWork wuow(opCtx);
        repl::OpTime opTime = repl::logOp(opCtx, &oplogEntry);
        uassert(12120908,
                str::stream() << "Failed to create new oplog entry for "
                                 "setAllowChunkOperations with opTime: "
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
    auto thisShardHandle = ShardingState::get(opCtx)->shardHandle();
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

    CollectionMetadata ownedMetadata(CurrentChunkManager(std::move(rtHandle)), thisShardHandle);

    auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, nss);
    scopedCsr->setAuthoritative();
    scopedCsr->setCollectionMetadata(opCtx, std::move(ownedMetadata));

    // Update allowChunkOperations and write an oplog 'c' entry to send the new allowChunkOperations
    // value to secondaries, since its value could have potentially changed.
    // TODO (SERVER-127444): remove these lines.
    scopedCsr->setAllowChunkOperations(coll.getAllowChunkOperations());
    setAllowChunkOperationsOnSecondaries(
        opCtx, nss, coll.getUuid(), coll.getAllowChunkOperations());
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

    // Write an oplog 'c' entry to invalidate the CSR and clear it on this node and secondaries.
    invalidateCollectionMetadata(opCtx, nss, uuid, true /* forDroppedCollection */);
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
            // Emit the oplog 'c' entry and clear the in-memory state.
            invalidateCollectionMetadata(
                opCtx, fromNss, *fromUUID, true /* forDroppedCollection */);
        }

        if (clearToNss && targetUUID) {
            // Emit the oplog 'c' entry and clear the in-memory state.
            invalidateCollectionMetadata(
                opCtx, toNss, *targetUUID, true /* forDroppedCollection */);
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
                                     bool isDbPrimaryShard,
                                     const boost::optional<UUID>& uuid,
                                     boost::optional<bool> expectedAllowChunkOperations) {
    auto coll = fetchCollection(opCtx, nss);

    uassert(ErrorCodes::InvalidUUID,
            fmt::format(
                "Collection uuid {} in the request does not match the current uuid {} for ns {}",
                uuid->toString(),
                coll.getUuid().toString(),
                nss.toStringForErrorMsg()),
            !uuid || uuid == coll.getUuid());
    tassert(12120907,
            "Retrieved allowChunkOperations from CSRS doesn't match",
            !expectedAllowChunkOperations ||
                coll.getAllowChunkOperations() == *expectedAllowChunkOperations);

    const auto ownedChunks = fetchOwnedChunks(opCtx, nss, coll);

    // Drop any prior chunk entries for this collection so repeated calls (e.g. when an unsplittable
    // collection is sharded) don't accumulate stale rows. The new chunk documents may carry
    // different OIDs, in which case writeCollectionMetadataLocally's upsert-by-OID would insert
    // rather than replace.
    deleteCollectionChunksMetadataLocally(opCtx, coll.getUuid());

    const bool hasCollectionEntry = isDbPrimaryShard || !ownedChunks.empty();
    if (hasCollectionEntry) {
        // Write to `config.shard.catalog.(collections|chunks)` to insert collection metadata.
        writeCollectionMetadataLocally(opCtx, nss, coll.asShardCatalogType(), ownedChunks);
    } else {
        // If the shard owns no chunks AND is not the dbPrimary then it really doesn't know anything
        // about the collection. Make sure to delete any existing collection entry.
        deleteCollectionEntryLocally(opCtx, nss);
    }

    // Emit the oplog 'c' entry and clear the in-memory state.
    invalidateCollectionMetadata(
        opCtx, nss, coll.getUuid(), !hasCollectionEntry /* forDroppedCollection */);

    if (hasCollectionEntry) {
        // Update this node CSR with collection metadata and chunks as an optimization, so the next
        // query doesn't have to recover it from disk.
        updateShardCatalogCache(opCtx, nss, coll, ownedChunks);
    }
}

void cloneCollectionMetadataLocally(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    bool isDbPrimaryShard) {
    auto coll = fetchCollection(opCtx, nss);

    const auto ownedChunks = fetchOwnedChunks(opCtx, nss, coll);

    // Drop any prior chunk entries for this collection so repeated calls don't accumulate stale
    // rows (see commitCollectionMetadataLocally for details).
    deleteCollectionChunksMetadataLocally(opCtx, coll.getUuid());

    if (isDbPrimaryShard || !ownedChunks.empty()) {
        // Write to `config.shard.catalog.(collections|chunks)` to insert collection metadata.
        writeCollectionMetadataLocally(opCtx, nss, coll.asShardCatalogType(), ownedChunks);
    } else {
        // The shard owns no chunks and is not the dbPrimary, so it doesn't track the collection.
        deleteCollectionEntryLocally(opCtx, nss);
    }
}

void commitChunklessCollectionMetadataLocally(OperationContext* opCtx, const NamespaceString& nss) {
    auto coll = fetchCollection(opCtx, nss);

    // Upsert the collection entry into the shard catalog. A concurrent migration may issue the same
    // upsert, but because it writes an identical document the two operations do not conflict.
    upsertCollectionEntryLocally(opCtx, nss, coll);

    // There is no need to invalidate or clear the in-memory filtering metadata here, except for a
    // specific case:
    //   - If the CSR is non-authoritative, it will become authoritative later, and will sort ifself
    //     out.
    //   - If this shard owns chunks, its CSS is tracked (or unknown if it has not been refreshed
    //     yet). No need to do anything.
    //   - If this shard owns no chunks, then the CSS would be either unknown or unowned. In the
    //     latter case, we need to invalidate it so that the CSS is recreated with state "tracked"
    //     (a DB primary can't have "kUnowned" entries in the CSS by definition).

    const bool isUnowned = [&] {
        // This lock is dropped before invalidating because the op observer has to reacquire it to
        // clear the CSR. This is fine: at worst, it forces a redundant re-recovery.
        const auto scopedCsr = CollectionShardingRuntime::acquireShared(opCtx, nss);
        return scopedCsr->isUnowned();
    }();
    if (isUnowned) {
        invalidateCollectionMetadata(opCtx, nss, coll.getUuid(), false /* forDroppedCollection */);
    }
}

void commitSetAllowChunkOperationsLocally(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          bool allowChunkOperations,
                                          const boost::optional<UUID>& uuid,
                                          bool isPrimaryShard) {
    const auto [metadata, isAuthoritative, isUnowned] = [&] {
        const auto csr = CollectionShardingRuntime::acquireShared(opCtx, nss);
        return std::make_tuple(csr->getCurrentMetadataIfKnown(),
                               csr->getAuthoritativeState() ==
                                   CollectionShardingRuntime::AuthoritativeState::kAuthoritative,
                               csr->isUnowned());
    }();

    if (isAuthoritative) {
        // The in-memory metadata could be unknown, throwing this StaleConfig forces a refresh.
        uassert(StaleConfigInfo(nss,
                                *OperationShardingState::get(opCtx).getShardVersion(nss),
                                boost::none /* wantedVersion */,
                                ShardingState::get(opCtx)->asShardRef(opCtx)),
                fmt::format("commitSetAllowChunkOperationsLocally: collection metadata for {} is "
                            "not currently known and needs to be recovered",
                            nss.toStringForErrorMsg()),
                metadata);

        if (isUnowned || !metadata->hasRoutingTable()) {
            // If the collection is unowned, we know that we don't own metadata for this collection
            // at all, so just do nothing.
            // If the collection doesn't have a routing table, then it's either untracked or
            // unsplittable, in any case just return.
            return;
        }

        uassert(
            ErrorCodes::InvalidUUID,
            fmt::format(
                "Collection uuid {} in the request does not match the current uuid {} for ns {}",
                uuid->toString(),
                metadata->getUUID().toString(),
                nss.toStringForErrorMsg()),
            !uuid || uuid == metadata->getUUID());

        writeCollectionAllowChunkOperationsLocally(opCtx, nss, allowChunkOperations);

        // Write an oplog 'c' entry to send the new value to secondaries.
        setAllowChunkOperationsOnSecondaries(opCtx, nss, metadata->getUUID(), allowChunkOperations);

        auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, nss);
        scopedCsr->setAllowChunkOperations(allowChunkOperations);

        LOGV2(12120905,
              "commitSetAllowChunkOperationsLocally authoritatively",
              "nss"_attr = nss,
              "allowChunkOperations"_attr = allowChunkOperations);
    } else {
        commitCollectionMetadataLocally(opCtx, nss, isPrimaryShard, uuid, allowChunkOperations);
        LOGV2(12120906,
              "commitSetAllowChunkOperationsLocally non-authoritatively",
              "nss"_attr = nss,
              "allowChunkOperations"_attr = allowChunkOperations);
    }
}

}  // namespace shard_catalog_commit

namespace shard_catalog_commit_for_resharding {
void commitCreateCollection(OperationContext* opCtx,
                            const NamespaceString& tempReshardingNss,
                            bool isDbPrimaryShard) {
    return shard_catalog_commit::commitCollectionMetadataLocally(
        opCtx, tempReshardingNss, isDbPrimaryShard);
}

void commitDropCollection(OperationContext* opCtx, const NamespaceString& nss, const UUID& uuid) {
    return shard_catalog_commit::commitDropCollectionLocally(opCtx, nss, uuid);
}

void commitRenameOfTemporaryCollection(OperationContext* opCtx,
                                       const NamespaceString& tempReshardingNss,
                                       const UUID& tempReshardingUUID,
                                       const NamespaceString& sourceNss,
                                       const UUID& sourceUUID,
                                       bool isUpgrading,
                                       bool isDbPrimaryShard) {
    return shard_catalog_commit::commitRenameOfCollectionMetadata(opCtx,
                                                                  tempReshardingNss,
                                                                  tempReshardingUUID,
                                                                  sourceNss,
                                                                  sourceUUID,
                                                                  boost::none,
                                                                  isUpgrading,
                                                                  isDbPrimaryShard);
}

namespace {
void ensureCollectionDoesNotExist(OperationContext* opCtx, const UUID& uuid) {
    try {
        auto catalogClient = Grid::get(opCtx)->catalogClient();
        const auto _ = catalogClient->getCollection(opCtx, uuid);
        tasserted(ErrorCodes::IllegalOperation,
                  "Collection entry must not exist in the global catalog to drop stale chunks");
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // This is actually the expected path.
    }
}
}  // namespace

void commitDropOfStaleChunksForRename(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const UUID& oldUuid) {
    ensureCollectionDoesNotExist(opCtx, oldUuid);

    // This function gets called without holding a critical section but the DDL lock as well as
    // migrations are disabled. The order of operations here is quite deliberate in order to make
    // PIT readers view either the entire old metadata (and therefore let the read through with a
    // consistent view of the data) or none (and thus fail the read):
    // * Dropping the collection entry first means that either the disk recovery sees all previous
    //   metadata or nothing.
    // * The invalidate is done first in order to inform concurrent recoverers that the data they
    //   read is no longer valid.
    // * And finally the last clear ensures that if any recovery succeeded before the invalidate got
    //   received by the recoverer then it will leave the CSR in the cleared state for future
    //   readers.
    //
    // This is exclusively a problem for PIT readers on shards that no longer own any chunk and
    // getting called by a stale router after resharding since:
    // 1. Owning shards will trigger a StaleConfig to the stale router.
    // 2. PIT readers of shards that don't own chunks will see either the correct documents
    //    (pre-resharding) or none at all as per the comment above.
    // 3. non-PIT readers would see nothing if this shard has no owned chunks and therefore doesn't
    //    matter whether the metadata is stale or not since the end result is the same on both
    //    scenarios (no documents).
    {
        DBDirectClient dbClient(opCtx);
        shard_catalog_commit::executeLocalDelete(
            dbClient,
            NamespaceString::kConfigShardCatalogCollectionsNamespace,
            BSON(CollectionType::kUuidFieldName << oldUuid),
            true /* multi */);
    }
    shard_catalog_commit::commitDropOfStaleChunksForRename(opCtx, oldUuid);

    // This invalidation will only be done by shards that were only donors or shards that didn't
    // participate in resharding. On both of these it's safe to do an invalidation since there's no
    // data present on the shard.
    bool shardOwnsChunks = shard_catalog_commit::doesShardOwnChunks(opCtx, nss);
    if (!shardOwnsChunks) {
        shard_catalog_commit::invalidateCollectionMetadata(
            opCtx, nss, oldUuid, true /* forDroppedCollection */);
    }
}

}  // namespace shard_catalog_commit_for_resharding

}  // namespace mongo
