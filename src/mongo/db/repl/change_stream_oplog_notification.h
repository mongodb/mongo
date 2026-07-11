// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/notify_sharding_event_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <set>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Writes a no-op oplog entry on shardCollection event.
 */
void notifyChangeStreamsOnShardCollection(OperationContext* opCtx,
                                          const CollectionSharded& notification);

/**
 * Builds no-op oplog entry corresponding to movePrimary event.
 */
repl::MutableOplogEntry buildMovePrimaryOplogEntry(OperationContext* opCtx,
                                                   const DatabaseName& dbName,
                                                   const ShardId& oldPrimary,
                                                   const ShardId& newPrimary);

/**
 * Writes a no-op oplog entry on movePrimary event.
 */
void notifyChangeStreamsOnMovePrimary(OperationContext* opCtx,
                                      const DatabaseName& dbName,
                                      const ShardId& oldPrimary,
                                      const ShardId& newPrimary);

/**
 * Writes a no-op oplog entry to match the completion of a reshardCollection operation.
 */
void notifyChangeStreamsOnReshardCollectionComplete(
    OperationContext* opCtx, const CollectionResharded& CollectionReshardedNotification);

/**
 * Builds no-op oplog entries corresponding to the completion of moveChunk/moveRange operation.
 * Builds up to three oplog entries:
 * - moveChunk
 * - migrateLastChunkFromShard
 * - migrateChunkToNewShard
 */
std::vector<repl::MutableOplogEntry> buildMoveChunkOplogEntries(
    OperationContext* opCtx,
    const NamespaceString& collName,
    const boost::optional<UUID>& collUUID,
    const ShardId& donor,
    const ShardId& recipient,
    bool noMoreCollectionChunksOnDonor,
    bool firstCollectionChunkOnRecipient);

/**
 * Writes a a series of no-op oplog entries to match the completion of a moveChunk/moveRange
 * operation.
 */
void notifyChangeStreamsOnChunkMigrated(OperationContext* opCtx,
                                        const NamespaceString& collName,
                                        const boost::optional<UUID>& collUUID,
                                        const ShardId& donor,
                                        const ShardId& recipient,
                                        bool noMoreCollectionChunksOnDonor,
                                        bool firstCollectionChunkOnRecipient);

/**
 * Builds no-op oplog entry corresponding to NamespacePlacementChanged notification.
 */
repl::MutableOplogEntry buildNamespacePlacementChangedOplogEntry(
    OperationContext* opCtx, const NamespacePlacementChanged& notification);

/**
 * Writes a no-op oplog entry concerning the commit of a generic placement-changing operation
 * concerning the namespace and the cluster time reported in the notification.
 */
void notifyChangeStreamsOnNamespacePlacementChanged(OperationContext* opCtx,
                                                    const NamespacePlacementChanged& notification);

/**
 * Writes a no-op oplog entry concerning the commit of an operation
 * modifying the operational boundaries of config.placementHistory.
 */
void notifyChangeStreamsOnPlacementHistoryMetadataChanged(
    OperationContext* opCtx, const PlacementHistoryMetadataChanged& notification);

/**
 * Writes a no-op oplog entry on the end of multi shard transaction.
 **/
void notifyChangeStreamOnEndOfTransaction(OperationContext* opCtx,
                                          const LogicalSessionId& lsid,
                                          const TxnNumber& txnNumber,
                                          const std::vector<NamespaceString>& affectedNamespaces);

/**
 * Writes a no-op oplog entry when refinement of collection shard key is complete.
 */
void notifyChangeStreamsOnRefineCollectionShardKeyComplete(OperationContext* opCtx,
                                                           const NamespaceString& collNss,
                                                           const KeyPattern& shardKey,
                                                           const KeyPattern& oldShardKey,
                                                           const UUID& collUUID);

/**
 * Writes a no-op oplog entry when a temporary resharding collection is created on a donor shard.
 */
void notifyChangeStreamsOnReshardCollectionBegin(OperationContext* opCtx,
                                                 const NamespaceString& sourceNss,
                                                 const UUID& sourceUUID,
                                                 const UUID& reshardingUUID);

/**
 * Writes one no-op oplog entry per recipient shard when writes are temporarily blocked for
 * resharding.
 */
void notifyChangeStreamsOnReshardCollectionBlockingWrites(
    OperationContext* opCtx,
    const NamespaceString& sourceNss,
    const UUID& sourceUUID,
    const UUID& reshardingUUID,
    const std::vector<ShardId>& recipientShardIds);

/**
 * Writes a no-op oplog entry when the temporary resharding collection has reached strict
 * consistency on a recipient shard.
 */
void notifyChangeStreamsOnReshardCollectionStrictConsistency(OperationContext* opCtx,
                                                             const NamespaceString& tempNss,
                                                             const UUID& reshardingUUID);

}  // namespace mongo
