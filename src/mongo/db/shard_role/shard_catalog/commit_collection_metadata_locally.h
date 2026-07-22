// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {

namespace [[MONGO_MOD_PARENT_PRIVATE]] shard_catalog_commit {

void logShardCatalogCommandOplogEntry(OperationContext* opCtx,
                                      repl::MutableOplogEntry& oplogEntry,
                                      const char* opName);

/**
 * Deletes the collection and chunk metadata from the shard catalog
 * (config.shard.catalog.collections and config.shard.catalog.chunks), writes an oplog entry to
 * invalidate collection metadata on secondaries, and clears the in-memory CollectionShardingRuntime
 * (CSR) for the dropped collection.
 */
void commitDropCollectionLocally(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const UUID& uuid);

/**
 * Deletes only chunk metadata from the shard catalog (config.shard.catalog.chunks) and does nothing
 * else. Only used by rename in the event of rename overwriting an existing collection.
 */
void commitDropOfStaleChunksForRename(OperationContext* opCtx, const UUID& uuid);

/**
 * Modifies the shard catalog for both fromNss and toNss in order to durably persist the decision to
 * rename the collection.
 * The source namespace metadata is invalidated and cleared. The target namespace filtering metadata
 * is recovered in-memory from the (already durable) local shard catalog so that it does not have to
 * be repopulated on the next query, while secondaries are signalled to recover it lazily. When the
 * commit is forced to behave as an FCV upgrade, the full metadata is instead re-fetched from the
 * global catalog.
 */
void commitRenameOfCollectionMetadata(OperationContext* opCtx,
                                      const NamespaceString& fromNss,
                                      const boost::optional<UUID>& fromUUID,
                                      const NamespaceString& toNss,
                                      const boost::optional<UUID>& targetUUID,
                                      const boost::optional<UUID>& newTargetUUID,
                                      bool isUpgrading,
                                      bool isDbPrimaryShard);
/**
 * Performs the local persistence of up-to-date collection metadata and chunk information for a
 * sharded collection on the shard. Specifically:
 *   1. Removes any existing chunk entries for the specified collection from the shard catalog
 *      (config.shard.catalog.chunks).
 *   2. Fetches the latest collection metadata and owned chunk entries from the global catalog.
 *   3. Persists the collection metadata and owned chunks to the shard catalog collections and
 * chunks namespaces (config.shard.catalog.collections and config.shard.catalog.chunks).
 *   4. Writes an oplog entry to invalidate collection metadata on secondaries.
 *   5. Updates the in-memory CollectionShardingRuntime (CSR) to reflect the new filtering
 * information.
 */
void commitCollectionMetadataLocally(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     bool isDbPrimaryShard = false);

/**
 * Persists collection and chunk metadata into the durable shard catalog during the setFCV
 * authoritative metadata clone.
 */
void cloneCollectionMetadataLocally(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    bool isDbPrimaryShard);

/**
 * Persists the collection entry, without registering any empty chunks for tracked collection.
 */
void commitChunklessCollectionMetadataLocally(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Commits the allowChunkOperations flag to the shard catalog (config.shard.catalog.collections).
 * Does nothing if the current shard has no tracked routing table for the collection.
 */
void commitSetAllowChunkOperationsLocally(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          bool allowChunkOperations,
                                          const boost::optional<UUID>& uuid);

/**
 * Commits an incremental chunk delta to the shard catalog given only the list of new chunk
 * documents. The shard reconciles overlaps with its pre-existing durable chunks
 * (config.shard.catalog.chunks) locally: any pre-existing chunk that overlaps a new chunk is
 * removed so the collection stays non-overlapping once the new chunks are inserted. After
 * persisting the delta, both the in-memory CollectionShardingRuntime and the secondaries (via an
 * oplog 'c' entry) are updated with the new chunks.
 *
 * When receivingFirstChunk is true, this shard owned no chunks for the collection before this
 * operation, so there is no valid in-memory base to apply the delta on top of. In that case the
 * collection metadata is bootstrapped from the global catalog (a full install that invalidates and
 * reinstalls the metadata) instead of applying an incremental delta.
 */
void commitChunkOperationsMetadataLocally(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const std::vector<BSONObj>& newChunks,
                                          bool receivingFirstChunk = false);

}  // namespace shard_catalog_commit

/**
 * The following family of methods are only meant to be used by the resharding module due to the way
 * resharding doesn't compose with the existing coordinators. Instead it inlines the work done by
 * coordinators into the ReshardingCoordinator, and as such has to access the necessary methods for
 * authoritative shard catalog changes.
 */
namespace [[MONGO_MOD_PUBLIC]] shard_catalog_commit_for_resharding {
void commitCreateCollection(OperationContext* opCtx,
                            const NamespaceString& tempReshardingNss,
                            bool isDbPrimaryShard);

void commitDropCollection(OperationContext* opCtx, const NamespaceString& nss, const UUID& uuid);

void commitRenameOfTemporaryCollection(OperationContext* opCtx,
                                       const NamespaceString& tempReshardingNss,
                                       const UUID& tempReshardingUUID,
                                       const NamespaceString& sourceNss,
                                       const UUID& sourceUUID,
                                       bool isDbPrimaryShard);

void commitDropOfStaleChunksForRename(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const UUID& oldUuid);
}  // namespace shard_catalog_commit_for_resharding
}  // namespace mongo
