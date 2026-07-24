// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/collection_metadata_synchronizer.h"

#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/sharding_catalog_client_impl.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/read_concern_mongod_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

CollectionMetadata readCollectionMetadataFromDisk(OperationContext* opCtx,
                                                  boost::optional<repl::OpTime> timestampToReadAt,
                                                  const NamespaceString& nss) {
    if (timestampToReadAt) {
        // Setup the snapshot timestamp on the opCtx.
        repl::ReadConcernArgs::get(opCtx) =
            repl::ReadConcernArgs::snapshot(timestampToReadAt->getTimestamp());
    } else {
        // Queryable backup mode is a frozen standalone snapshot. There is no live replication
        // timestamp to wait for, so read from the local catalog as it exists on disk.
        repl::ReadConcernArgs::get(opCtx) = repl::ReadConcernArgs{};
    }

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
            "Failed to establish a cursor while reading the collection metadata from the shard "
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
    return CollectionMetadata{std::move(cm), ShardingState::get(opCtx)->shardId()};
}

}  // namespace

void CollectionMetadataSynchronizer::start(OperationContext* opCtx, ExecutorPtr executor) {
    std::lock_guard lk(_mutex);

    if (_collMetadata.valid()) {
        // start() was already called.
        return;
    }

    // We first have to wait until the lastWritten batch of oplog entries has been applied and can
    // be used for snapshot reads. This is because not doing so exposes the synchronizer to a race
    // condition with oplog application.
    //
    // Consider the following scenario:
    //  * A secondary has fetched an oplog batch containing oplog 'c' entries for the CSS
    //  * The secondary applies the batch
    //  * Concurrently we also start the metadata synchronizer
    // Now the last two operations can race in such a way that the synchronizer would use a
    // timestamp from before the oplog 'c' entry was committed to the oplog and that entry becomes
    // lost since it was applied before the synchronizer was installed. As a result, the
    // synchronizer would end up installing the wrong CSS state since that entry would not be
    // present in the queue even if it logically came after the timestamp used by the synchronizer.
    //
    // To prevent this scenario we write down the lastWritten timestamp after installing the
    // synchronizer. This will ensure that all oplog entries that come after will see the
    // synchronizer in place and enqueue themselves.
    _timestampToReadAt = repl::ReplicationCoordinator::get(opCtx)->getMyLastWrittenOpTime();
    LOGV2_INFO(12195302,
               "Reading collection sharding metadata from disk",
               "nss"_attr = _nss,
               "timestampToReadAt"_attr = _timestampToReadAt);

    if (storageGlobalParams.queryableBackupMode || gTestingSnapshotBehaviorInIsolation) {
        // Queryable backup mode and testingSnapshotBehaviorInIsolation do not advance the
        // replication committed snapshot, so read from the local catalog as it exists on disk.
        _collMetadata = SemiFuture<CollectionMetadata>::makeReady(
                            readCollectionMetadataFromDisk(opCtx, boost::none, _nss))
                            .share();
        return;
    }

    _collMetadata =
        future_util::withCancellation(
            repl::ReplicationCoordinator::get(opCtx)->registerWaiterForMajorityReadOpTime(
                opCtx, _timestampToReadAt),
            _cancellationSource.token())
            .thenRunOn(executor)
            .then([nss = _nss,
                   token = _cancellationSource.token(),
                   timestampToReadAt = _timestampToReadAt,
                   executor] {
                ThreadClient client{"MetadataSynchronizer",
                                    getGlobalServiceContext()->getService()};
                auto opCtx =
                    CancelableOperationContext{client->makeOperationContext(), token, executor};

                // Collection metadata is critical for queries, so mark this opCtx as
                // NonDeprioritizable to avoid admission-control deprioritization.
                admission::execution_control::ScopedTaskTypeNonDeprioritizable deprioGuard(
                    opCtx.get());

                LOGV2_INFO(12033903,
                           "Wait for stable timestamp finished, proceeding to read disk contents",
                           "collection"_attr = nss.toStringForErrorMsg());
                auto metadata = readCollectionMetadataFromDisk(opCtx.get(), timestampToReadAt, nss);
                LOGV2_INFO(12033902,
                           "Disk contents have been read",
                           "collection"_attr = nss.toStringForErrorMsg());
                return metadata;
            })
            .onCompletion([nss = _nss](const auto& status) {
                if (!status.isOK()) {
                    LOGV2_WARNING(12033901,
                                  "Encountered failure while reading collection metadata from disk",
                                  "error"_attr = status.getStatus(),
                                  "collection"_attr = nss.toStringForErrorMsg());
                }
                return status;
            })
            .share();
}

SharedSemiFuture<CollectionMetadata> CollectionMetadataSynchronizer::getMetadataFuture() const {
    std::lock_guard lk(_mutex);
    tassert(13119100, "Metadata future not installed before getting", _collMetadata.valid());
    return _collMetadata;
}

void CollectionMetadataSynchronizer::onOplogEntry(
    Timestamp entryTs, const InvalidateCollectionMetadataOplogEntry& entry) {
    LOGV2_INFO(12195301,
               "Received oplog entry for invalidation",
               "nss"_attr = _nss,
               "timestamp"_attr = entryTs);

    std::lock_guard lk(_mutex);
    if (entryTs <= _timestampToReadAt.getTimestamp()) {
        return;
    }
    _entriesToApply.emplace(entryTs, entry);
}

void CollectionMetadataSynchronizer::onOplogEntry(Timestamp entryTs,
                                                  const UpdateCollectionMetadataOplogEntry& entry) {
    LOGV2_INFO(12195300,
               "Received oplog entry for delta application",
               "nss"_attr = _nss,
               "timestamp"_attr = entryTs);

    std::lock_guard lk(_mutex);
    if (entryTs <= _timestampToReadAt.getTimestamp()) {
        return;
    }
    _entriesToApply.emplace(entryTs, entry);
}

namespace {
boost::optional<CollectionMetadata> applyOplogEntry(OperationContext* opCtx,
                                                    const UpdateCollectionMetadataOplogEntry& entry,
                                                    CollectionMetadata collMetadata) {
    tassert(12698703,
            "Expected metadata from disk to have a routing table when applying changed chunks",
            collMetadata.hasRoutingTable());

    const auto collPlacementVersion = collMetadata.getCollPlacementVersion();
    return collMetadata.makeUpdated(
        ChunkType::parseConfigBSONDocuments(entry.getChangedChunks(),
                                            collMetadata.getUUID(),
                                            collPlacementVersion.epoch(),
                                            collPlacementVersion.getTimestamp()));
}

boost::optional<CollectionMetadata> applyOplogEntry(
    OperationContext* opCtx,
    const InvalidateCollectionMetadataOplogEntry& entry,
    CollectionMetadata collMetadata) {
    // While the synchronizer is in progress we always force another round, regardless of the
    // entry's precondition. The precondition is meant to be evaluated against a node's stable,
    // installed collection metadata, but here the metadata is still being rebuilt from disk and is
    // not yet installed, so it cannot be evaluated reliably.
    return boost::none;
}
}  // namespace

boost::optional<CollectionMetadata> CollectionMetadataSynchronizer::drainAndApply(
    OperationContext* opCtx) {
    std::lock_guard lk(_mutex);

    tassert(13119101, "Metadata future not installed before draining", _collMetadata.valid());

    auto collMetadata = _collMetadata.get();
    for (; !_entriesToApply.empty(); _entriesToApply.pop()) {
        const auto& [ts, entry] = _entriesToApply.front();
        if (ts <= _timestampToReadAt.getTimestamp()) {
            continue;  // Already reflected in the disk snapshot.
        }

        auto updated = std::visit(
            [&](const auto& e) { return applyOplogEntry(opCtx, e, collMetadata); }, entry);
        if (!updated) {
            return boost::none;  // An invalidate forces a new synchronizer round.
        }
        collMetadata = std::move(*updated);
    }
    return collMetadata;
}

}  // namespace mongo
