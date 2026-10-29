// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
namespace catalog {

/**
 * Returns ErrorCodes::NamespaceExists if a collection or any type of views exists on the given
 * namespace 'nss'. Otherwise returns Status::OK().
 *
 * Note: If the caller calls this method without locking the collection, then the returned result
 * could be stale right after this call.
 */
[[MONGO_MOD_PUBLIC]]
Status checkIfNamespaceExists(OperationContext* opCtx, const NamespaceString& nss);

enum class [[MONGO_MOD_PUBLIC]] CollectionCatalogIterationResult {
    // No collection in the catalog matched the predicate.
    kNoMatches,
    // At least one collection in the catalog matched the predicate.
    // This may be returned even if the callback is never called,
    // if a collection that matched the predicate got concurrently dropped.
    kSomeMatches,
};

/**
 * Iterates through all the collections in the given database and runs the callback function on each
 * collection. If a predicate is provided, then the callback will only be executed against the
 * collections that satisfy the predicate.
 *
 * Additionally, no collection lock is held while checking the outcome of the predicate. The
 * predicate must not block, as an internal collection catalog mutex is held during its evaluation.
 * The collection lock is acquired when executing the callback only on the satisfying collections.
 *
 * Iterating through the remaining collections stops when the callback returns false.
 */
[[MONGO_MOD_PUBLIC]]
CollectionCatalogIterationResult forEachCollectionFromDb(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    LockMode collLockMode,
    CollectionCatalog::CollectionInfoFn callback,
    CollectionCatalog::CollectionInfoFn predicate = nullptr);


/**
 * Like forEachCollectionFromDb but iterates all databases using a single catalog snapshot taken
 * before any locks are acquired. This prevents a collection renamed across databases from dodging
 * the scan entirely when the rename happens between two per-DB iterations.
 *
 * Unlike forEachCollectionFromDb, this function acquires the database lock internally in addition
 * to the per-collection lock (at collLockMode). The database lock mode is derived automatically:
 * MODE_IS when collLockMode is a shared mode, MODE_IX otherwise. Callers must not hold any
 * database lock when calling this function.
 *
 * If a predicate is provided, the callback will only be executed against collections that
 * satisfy it. The predicate must not block, as an internal collection catalog mutex is held
 * during its evaluation.
 *
 * Iterating through the remaining collections stops when the callback returns false.
 */
[[MONGO_MOD_PUBLIC]]
CollectionCatalogIterationResult forEachCollectionFromAllDbs(
    OperationContext* opCtx,
    LockMode collLockMode,
    CollectionCatalog::CollectionInfoFn callback,
    CollectionCatalog::CollectionInfoFn predicate = nullptr);

/**
 * Iterates through all collections in all databases that satisfy the predicate and runs the
 * callback function on each collectiom, which should modify it to no longer satisfy the predicate.
 *
 * This function differs from `forEachCollectionFromDb` in that it will ensures if a collection
 * matching the predicate is cloned while iterating the catalog, it is also modified. Thus,
 * it ensures that the catalog converges to a state where no collection satisfies the predicate.
 */
[[MONGO_MOD_PUBLIC]]
void modifyAllCollectionsMatching(OperationContext* opCtx,
                                  std::function<void(const Collection* collection)> callback,
                                  CollectionCatalog::CollectionInfoFn predicate);

/**
 * Checks whether the specified namespace should be included in the debug dump of the config
 * collections.
 */
[[MONGO_MOD_PUBLIC]]
boost::optional<bool> getConfigDebugDump(const VersionContext& vCtx, const NamespaceString& nss);

/**
 * Indicates whether the data drop (the data table) should occur immediately or be two-phased, which
 * delays data removal to support older PIT reads or rollback.
 */
enum class [[MONGO_MOD_PUBLIC]] DataRemoval {
    kImmediate,
    kTwoPhase,
};

/**
 * Performs two-phase index drop.
 *
 * Passthrough to durable_catalog::removeIndex to execute the first phase of drop by removing the
 * index catalog entry, then registers an onCommit hook to schedule the second phase of drop to
 * delete the index data. The 'dataRemoval' field can be used to specify whether the second phase of
 * drop, table data deletion, should run immediately or delayed: immediate deletion should only be
 * used for incomplete indexes, where the index build is the only accessor and the data will not be
 * needed for earlier points in time.
 *
 * Uses IndexCatalogEntry::getSharedIdent() shared_ptr to ensure that the second phase of drop (data
 * table drop) will not execute until no users of the index (shared owners) remain.
 * IndexCatalogEntry::getSharedIdent() is allowed to be a nullptr, in which case the caller
 * guarantees that there are no remaining users of the index. This handles situations wherein there
 * is no in-memory state available for an index, such as during repair.
 */
[[MONGO_MOD_PUBLIC]]
void removeIndex(OperationContext* opCtx,
                 std::string_view indexName,
                 Collection* collection,
                 std::shared_ptr<IndexCatalogEntry> entry,
                 DataRemoval dataRemoval = DataRemoval::kTwoPhase);

/**
 * Performs two-phase collection drop.
 *
 * Passthrough to durable_catalog::dropCollection to execute the first phase of drop by removing the
 * collection entry, then registers an onCommit hook to schedule the second phase of drop to delete
 * the collection data.
 *
 * Uses 'ident' shared_ptr to ensure that the second phase of drop (data table drop) will not
 * execute until no users of the collection record store (shared owners) remain. 'ident' is not
 * allowed to be nullptr.
 */
[[MONGO_MOD_PRIVATE]] Status dropCollection(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            RecordId collectionCatalogId,
                                            std::shared_ptr<Ident> ident);

/**
 * Deletes all data and metadata for a database.
 */
[[MONGO_MOD_PRIVATE]] Status dropDatabase(OperationContext* opCtx, const DatabaseName& dbName);

/**
 * Delete all collections with a name starting with collectionNamePrefix in a database.
 * To drop all collections regardless of prefix, use an empty string.
 */
[[MONGO_MOD_PUBLIC]]
Status dropCollectionsWithPrefix(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const std::string& collectionNamePrefix);

/**
 * Shuts down collection catalog and storage engine cleanly.
 * Set `memLeakAllowed` to true for faster shutdown.
 */
[[MONGO_MOD_PUBLIC]]
void shutDownCollectionCatalogAndGlobalStorageEngineCleanly(ServiceContext* service,
                                                            bool memLeakAllowed);

/**
 * Starts up storage engine.
 *
 * In most scenarios, startUpStorageEngineAndCollectionCatalog() should be used instead of this
 * function unless there is a specific reason to defer collection catalog initialization.
 */
[[MONGO_MOD_PUBLIC]]
StorageEngine::LastShutdownState startUpStorageEngine(OperationContext* opCtx,
                                                      StorageEngineInitFlags initFlags,
                                                      BSONObjBuilder* startupTimeElapsedBuilder);

/**
 * Initializes catalog (storage engine's MDB and CollectionCatalog). To be used in scenarios where
 * catalog initialization needs to be deferred after storage engine startup (via
 * startUpStorageEngine).
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]]
void startUpCollectionCatalogDeferred(OperationContext* opCtx);

/**
 * Starts up storage engine and initializes the collection catalog.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]]
StorageEngine::LastShutdownState startUpStorageEngineAndCollectionCatalog(
    ServiceContext* service,
    Client* client,
    StorageEngineInitFlags initFlags = {},
    BSONObjBuilder* startupTimeElapsedBuilder = nullptr);

}  // namespace catalog
}  // namespace mongo
