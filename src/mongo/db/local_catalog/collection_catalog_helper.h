/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"

namespace mongo {

class Collection;
class CollectionPtr;
class CollectionCatalogEntry;

namespace catalog {

/**
 * Returns ErrorCodes::NamespaceExists if a collection or any type of views exists on the given
 * namespace 'nss'. Otherwise returns Status::OK().
 *
 * Note: If the caller calls this method without locking the collection, then the returned result
 * could be stale right after this call.
 */
Status checkIfNamespaceExists(OperationContext* opCtx, const NamespaceString& nss);

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
void forEachCollectionFromDb(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             LockMode collLockMode,
                             CollectionCatalog::CollectionInfoFn callback,
                             CollectionCatalog::CollectionInfoFn predicate = nullptr);

boost::optional<bool> getConfigDebugDump(const VersionContext& vCtx, const NamespaceString& nss);

/**
 * Indicates whether the data drop (the data table) should occur immediately or be two-phased, which
 * delays data removal to support older PIT reads or rollback.
 */
enum class DataRemoval {
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
void removeIndex(OperationContext* opCtx,
                 StringData indexName,
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
Status dropCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      RecordId collectionCatalogId,
                      std::shared_ptr<Ident> ident);

/**
 * Deletes all data and metadata for a database.
 */
Status dropDatabase(OperationContext* opCtx, const DatabaseName& dbName);

/**
 * Delete all collections with a name starting with collectionNamePrefix in a database.
 * To drop all collections regardless of prefix, use an empty string.
 */
Status dropCollectionsWithPrefix(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const std::string& collectionNamePrefix);

/**
 * Shuts down collection catalog and storage engine cleanly.
 * Set `memLeakAllowed` to true for faster shutdown.
 */
void shutDownCollectionCatalogAndGlobalStorageEngineCleanly(ServiceContext* service,
                                                            bool memLeakAllowed);

/**
 * Starts up storage engine.
 *
 * In most scenarios, startUpStorageEngineAndCollectionCatalog() should be used instead of this
 * function unless there is a specific reason to defer collection catalog initialization.
 */
StorageEngine::LastShutdownState startUpStorageEngine(OperationContext* opCtx,
                                                      StorageEngineInitFlags initFlags,
                                                      BSONObjBuilder* startupTimeElapsedBuilder);

/**
 * Starts up storage engine and initializes the collection catalog.
 */
StorageEngine::LastShutdownState startUpStorageEngineAndCollectionCatalog(
    ServiceContext* service,
    Client* client,
    StorageEngineInitFlags initFlags = {},
    BSONObjBuilder* startupTimeElapsedBuilder = nullptr);
}  // namespace catalog
}  // namespace mongo
