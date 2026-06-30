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

#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/sharding_catalog_client_impl.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/read_concern_mongod_gen.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

CollectionMetadata recoverCollectionFromDisk(OperationContext* opCtx,
                                             boost::optional<repl::OpTime> timestampToReadAt,
                                             const NamespaceString& nss) {
    if (timestampToReadAt) {
        // Setup the snapshot timestamp on the opCtx.
        repl::ReadConcernArgs::get(opCtx) =
            repl::ReadConcernArgs{LogicalTime{timestampToReadAt->getTimestamp()},
                                  repl::ReadConcernLevel::kSnapshotReadConcern};
    } else {
        // Queryable backup mode is a frozen standalone snapshot. There is no live replication
        // timestamp to wait for, so recover from the local catalog as it exists on disk.
        repl::ReadConcernArgs::get(opCtx) = repl::ReadConcernArgs{};
    }

    // TODO (SERVER-128431): Investigate if we can avoid the full refresh by passing the current sv.
    auto aggRequest =
        makeCollectionAndChunksAggregation(opCtx,
                                           NamespaceString::kConfigShardCatalogCollectionsNamespace,
                                           NamespaceString::kConfigShardCatalogChunksNamespace,
                                           nss,
                                           ChunkVersion::UNTRACKED());

    std::vector<BSONObj> aggResult;
    {
        DBDirectClient client{opCtx};
        auto cursor = uassertStatusOKWithContext(
            DBClientCursor::fromAggregationRequest(
                &client, aggRequest, false /* secondaryOk */, true /* useExhaust */),
            "Failed to establish a cursor while recovering the collection metadata from the shard "
            "catalog");
        while (cursor->more()) {
            aggResult.emplace_back(cursor->nextSafe().getOwned());
        }
    }

    // No collection document means the collection is untracked, as the presence of an entry implies
    // that the collection is tracked on the sharding catalog.
    boost::optional<CollectionType> coll;
    for (const auto& elem : aggResult) {
        if (!elem.getField("chunks")) {
            coll.emplace(elem);
            break;
        }
    }
    if (!coll) {
        return CollectionMetadata::UNTRACKED();
    }

    std::vector<ChunkType> chunks;
    chunks.reserve(aggResult.size());
    for (const auto& elem : aggResult) {
        if (const auto chunkElem = elem.getField("chunks")) {
            chunks.emplace_back(uassertStatusOK(ChunkType::parseFromConfigBSON(
                chunkElem.Obj(), coll->getEpoch(), coll->getTimestamp())));
        }
    }

    auto defaultCollator = [&]() -> std::unique_ptr<CollatorInterface> {
        if (auto collation = coll->getDefaultCollation(); !collation.isEmpty()) {
            // The collation should have been validated upon collection creation
            return uassertStatusOK(
                CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation));
        }
        return nullptr;
    }();
    auto rt = OptionalRoutingTableHistory{std::make_shared<RoutingTableHistory>(
        RoutingTableHistory::makeNewAllowingGaps(coll->getNss(),
                                                 coll->getUuid(),
                                                 coll->getKeyPattern(),
                                                 coll->getUnsplittable(),
                                                 std::move(defaultCollator),
                                                 coll->getUnique(),
                                                 coll->getEpoch(),
                                                 coll->getTimestamp(),
                                                 coll->getTimeseriesFields(),
                                                 coll->getReshardingFields(),
                                                 coll->getAllowMigrations(),
                                                 chunks))};
    auto cm = CurrentChunkManager{std::move(rt)};
    return CollectionMetadata{std::move(cm), ShardingState::get(opCtx)->shardHandle()};
}
}  // namespace

Status CollectionCacheRecoverer::waitForInitialPass(
    OperationContext* opCtx, CollectionCacheRecoverer::RecoveryRoundId recoveryRound) {
    const auto [future, roundId] = [&] {
        std::lock_guard lk(_mutex);
        // We copy the future in order to avoid racing with a concurrent drainAndApply that will
        // reset the recoverer.
        return std::make_pair(_collMetadata, _timestampToReadAt);
    }();
    if (recoveryRound.id != roundId) {
        // The wait is invalid since the previous start failed to produce a result and we had to
        // reset everything. The future is potentially in an invalid state since disk recovery needs
        // to be restarted.
        return {ErrorCodes::AtomicityFailure, ""};
    }
    tassert(12033900,
            "Attempting to recover without a valid collection metadata setup or without "
            "setting up "
            "async recovery",
            future.valid());
    return future.getNoThrow(opCtx).getStatus();
}

CollectionCacheRecoverer::RecoveryRoundId CollectionCacheRecoverer::start(OperationContext* opCtx,
                                                                          ExecutorPtr executor) {
    std::lock_guard lk(_mutex);

    if (_collMetadata.valid()) {
        // If there's already a future in place it means we either got created with an existing set
        // of CollectionMetadata or someone else has called start(). In any case, there's nothing
        // for us to do here.
        return {_timestampToReadAt};
    }
    // We first have to wait until the lastWritten batch of oplog entries has been
    // applied and can be used for snapshot reads. This is because not doing so exposes
    // the recovery process to a race condition with oplog application.
    //
    // Consider the following scenario:
    // * A secondary has fetched an oplog batch containing oplog 'c' entries for the CSS
    // * The secondary applies the batch
    // * Concurrently we also initiate recovery from disk
    // Now the last two operations can race in such a way that recovery would use a timestamp
    // from before the oplog 'c' entry was committed to the oplog and that entry becomes lost
    // since it was applied before the recovery process was installed. As a result, the recovery
    // would end up installing the wrong CSS state since that entry would not be present in the
    // queue even if it logically came after the timestamp used by the recovery process.
    //
    // To prevent this scenario we write down the lastWritten timestamp after installing
    // the recovery process. This will ensure that all oplog entries that come after
    // will see the recovery process in place and enqueue themselves.
    _timestampToReadAt = repl::ReplicationCoordinator::get(opCtx)->getMyLastWrittenOpTime();
    LOGV2_INFO(12195302,
               "Recovering collection sharding metadata from disk",
               "nss"_attr = _nss,
               "recoveryTimestamp"_attr = _timestampToReadAt);

    if (storageGlobalParams.queryableBackupMode || gTestingSnapshotBehaviorInIsolation) {
        // Queryable backup mode and testingSnapshotBehaviorInIsolation do not advance the
        // replication committed snapshot, so recover from the local catalog as it exists on disk.
        _collMetadata = SemiFuture<CollectionMetadata>::makeReady(
                            recoverCollectionFromDisk(opCtx, boost::none, _nss))
                            .share();
        return {_timestampToReadAt};
    }

    _collMetadata =
        repl::ReplicationCoordinator::get(opCtx)
            ->registerWaiterForMajorityReadOpTime(opCtx, _timestampToReadAt)
            .thenRunOn(executor)
            .then([nss = _nss,
                   svcCtx = opCtx->getService(),
                   token = _cancellationSource.token(),
                   timestampToReadAt = _timestampToReadAt,
                   executor] {
                ThreadClient client{"CSR-Recovery", getGlobalServiceContext()->getService()};
                auto opCtx =
                    CancelableOperationContext{client->makeOperationContext(), token, executor};

                // Recovery of the filtering metadata is critical for queries, so mark this opCtx
                // as NonDeprioritizable to avoid admission-control deprioritization.
                admission::execution_control::ScopedTaskTypeNonDeprioritizable deprioGuard(
                    opCtx.get());

                LOGV2_INFO(12033903,
                           "Wait for stable timestamp finished, proceeding to read disk contents",
                           "collection"_attr = nss.toStringForErrorMsg());
                auto metadata = recoverCollectionFromDisk(opCtx.get(), timestampToReadAt, nss);
                LOGV2_INFO(12033902,
                           "Disk contents have been read",
                           "collection"_attr = nss.toStringForErrorMsg());
                return metadata;
            })
            .onCompletion(([nss = _nss](const auto& status) {
                if (!status.isOK()) {
                    LOGV2_WARNING(12033901,
                                  "Encountered failure during disk recovery",
                                  "error"_attr = status.getStatus(),
                                  "collection"_attr = nss.toStringForErrorMsg());
                }
                return status;
            }))
            .share();
    return {_timestampToReadAt};
}

void CollectionCacheRecoverer::onOplogEntry(Timestamp entryTs,
                                            const InvalidateCollectionMetadataOplogEntry& entry) {

    LOGV2_INFO(12195301,
               "Received oplog entry for invalidation",
               "nss"_attr = _nss,
               "timestamp"_attr = entryTs);
    std::lock_guard lk(_mutex);
    if (entryTs < _timestampToReadAt.getTimestamp()) {
        return;
    }
    _entriesToApply.emplace(entryTs, entry);
}

void CollectionCacheRecoverer::onOplogEntry(Timestamp entryTs,
                                            const CollectionShardingStateDeltaOplogEntry& entry) {
    LOGV2_INFO(12195300,
               "Received oplog entry for delta application",
               "nss"_attr = _nss,
               "timestamp"_attr = entryTs);
    std::lock_guard lk(_mutex);
    if (entryTs < _timestampToReadAt.getTimestamp()) {
        return;
    }
    _entriesToApply.emplace(entryTs, entry);
}

namespace {
boost::optional<CollectionMetadata> applyOplogEntry(
    OperationContext* opCtx,
    const CollectionShardingStateDeltaOplogEntry& entry,
    CollectionMetadata collMetadata) {
    tassert(12698703,
            "Expected recovered metadata to have a routing table when applying changed chunks",
            collMetadata.hasRoutingTable());

    const auto collPlacementVersion = collMetadata.getCollPlacementVersion();
    return collMetadata.makeUpdated(
        ChunkType::parseConfigBSONDocuments(entry.getChangedChunks(),
                                            collMetadata.getUUID(),
                                            collPlacementVersion.epoch(),
                                            collPlacementVersion.getTimestamp()),
        ShardingState::get(opCtx)->shardHandle());
}

boost::optional<CollectionMetadata> applyOplogEntry(
    OperationContext* opCtx,
    const InvalidateCollectionMetadataOplogEntry& entry,
    CollectionMetadata collMetadata) {
    return boost::none;
}
}  // namespace

boost::optional<CollectionMetadata> CollectionCacheRecoverer::drainAndApply(
    OperationContext* opCtx, CollectionCacheRecoverer::RecoveryRoundId recoveryRound) {
    std::lock_guard lk(_mutex);

    if (recoveryRound.id != _timestampToReadAt) {
        // The wait that preceeded this call was invalidated, we fail the drain on purpose since it
        // would otherwise block for a potentially long time to resolve the future and the state has
        // already been reset by the drain.
        return boost::none;
    }

    auto collMetadata = _collMetadata.get();
    while (!_entriesToApply.empty()) {
        ON_BLOCK_EXIT([&] { _entriesToApply.pop(); });
        const auto& [ts, entry] = _entriesToApply.front();
        auto newCollMetadata = std::visit(
            [&](const auto& entry) { return applyOplogEntry(opCtx, entry, collMetadata); }, entry);
        if (!newCollMetadata) {
            // Draining failed, signal the caller that it must perform another round of recovery. We
            // advance the timestamp such that it gets the new valid snapshot and reset the state of
            // _collMetadata so that we can restart.
            _timestampToReadAt = repl::OpTime{ts, repl::OpTime::kUninitializedTerm};
            _collMetadata = {};
            return boost::none;
        }
        collMetadata = std::move(*newCollMetadata);
    }
    return collMetadata;
}

}  // namespace mongo
