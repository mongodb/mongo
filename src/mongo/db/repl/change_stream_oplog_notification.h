/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/notify_sharding_event_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <set>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace MONGO_MOD_PUB mongo {

/*
 * This function writes a no-op oplog entry on shardCollection event.
 * TODO SERVER-66333: move all other notifyChangeStreams* functions here.
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
 * Writes a no-op oplog entry on the end of multi shard transaction.
 **/
void notifyChangeStreamOnEndOfTransaction(OperationContext* opCtx,
                                          const LogicalSessionId& lsid,
                                          const TxnNumber& txnNumber,
                                          const std::vector<NamespaceString>& affectedNamespaces);

}  // namespace MONGO_MOD_PUB mongo
