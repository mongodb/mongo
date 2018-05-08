/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#pragma once

#include <boost/optional.hpp>
#include <cstddef>
#include <iosfwd>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/repl/collection_bulk_loader.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"

namespace mongo {

class Collection;
struct CollectionOptions;
class OperationContext;

namespace repl {

struct TimestampedBSONObj {
    BSONObj obj;
    Timestamp timestamp;
};

/**
 * Storage interface used by the replication system to interact with storage.
 * This interface provides seperation of concerns and a place for mocking out test
 * interactions.
 *
 * The grouping of functionality includes general collection helpers, and more specific replication
 * concepts:
 *      * Create Collection and Oplog
 *      * Drop database and all user databases
 *      * Drop a collection
 *      * Insert documents into a collection
 */
class StorageInterface {
    MONGO_DISALLOW_COPYING(StorageInterface);

public:
    // Operation Context binding.
    static StorageInterface* get(ServiceContext* service);
    static StorageInterface* get(ServiceContext& service);
    static StorageInterface* get(OperationContext* opCtx);
    static void set(ServiceContext* service, std::unique_ptr<StorageInterface> storageInterface);

    // Constructor and Destructor.
    StorageInterface() = default;
    virtual ~StorageInterface() = default;

    /**
     * Rollback ID is an increasing counter of how many rollbacks have occurred on this server. It
     * is initialized with a value of 1, and should increase by exactly 1 every time a rollback
     * occurs.
     */

    /**
     * Return the current value of the rollback ID.
     */
    virtual StatusWith<int> getRollbackID(OperationContext* opCtx) = 0;

    /**
     * Initialize the rollback ID to 1. Returns the value of the initialized rollback ID if
     * successful.
     */
    virtual StatusWith<int> initializeRollbackID(OperationContext* opCtx) = 0;

    /**
     * Increments the current rollback ID. Returns the new value of the rollback ID if successful.
     */
    virtual StatusWith<int> incrementRollbackID(OperationContext* opCtx) = 0;


    // Collection creation and population for initial sync.
    /**
     * Creates a collection with the provided indexes.
     *
     * Assumes that no database locks have been acquired prior to calling this function.
     */
    virtual StatusWith<std::unique_ptr<CollectionBulkLoader>> createCollectionForBulkLoading(
        const NamespaceString& nss,
        const CollectionOptions& options,
        const BSONObj idIndexSpec,
        const std::vector<BSONObj>& secondaryIndexSpecs) = 0;

    /**
     * Inserts a document with a timestamp into a collection.
     *
     * NOTE: If the collection doesn't exist, it will not be created, and instead
     * an error is returned.
     */
    virtual Status insertDocument(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const TimestampedBSONObj& doc,
                                  long long term) = 0;

    /**
     * Inserts the given documents, with associated timestamps and statement id's, into the
     * collection.
     * It is an error to call this function with an empty set of documents.
     */
    virtual Status insertDocuments(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const std::vector<InsertStatement>& docs) = 0;

    /**
     * Creates the initial oplog, errors if it exists.
     */
    virtual Status createOplog(OperationContext* opCtx, const NamespaceString& nss) = 0;

    /**
     * Returns the configured maximum size of the oplog.
     *
     * Implementations are allowed to be "fuzzy" and delete documents when the actual size is
     * slightly above or below this, so callers should not rely on its exact value.
     */
    virtual StatusWith<size_t> getOplogMaxSize(OperationContext* opCtx,
                                               const NamespaceString& nss) = 0;

    /**
     * Creates a collection.
     */
    virtual Status createCollection(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const CollectionOptions& options) = 0;

    /**
     * Drops a collection.
     */
    virtual Status dropCollection(OperationContext* opCtx, const NamespaceString& nss) = 0;

    /**
     * Truncates a collection.
     */
    virtual Status truncateCollection(OperationContext* opCtx, const NamespaceString& nss) = 0;

    /**
     * Renames a collection from the "fromNS" to the "toNS". Fails if the new collection already
     * exists.
     */
    virtual Status renameCollection(OperationContext* opCtx,
                                    const NamespaceString& fromNS,
                                    const NamespaceString& toNS,
                                    bool stayTemp) = 0;

    /**
     * Drops all databases except "local".
     */
    virtual Status dropReplicatedDatabases(OperationContext* opCtx) = 0;

    /**
     * Validates that the admin database is valid during initial sync.
     */
    virtual Status isAdminDbValid(OperationContext* opCtx) = 0;

    /**
     * Finds at most "limit" documents returned by a collection or index scan on the collection in
     * the requested direction.
     * The documents returned will be copied and buffered. No cursors on the underlying collection
     * will be kept open once this function returns.
     * If "indexName" is boost::none, a collection scan is used to locate the document.
     * Index scan options:
     *     If "startKey" is not empty, the index scan will start from the given key (instead of
     *     MinKey/MaxKey).
     *     Set "boundInclusion" to BoundInclusion::kIncludeStartKeyOnly to include "startKey" in
     *     the index scan results. Set to BoundInclusion::kIncludeEndKeyOnly to return the key
     *     immediately following "startKey" from the index.
     */
    enum class ScanDirection {
        kForward = 1,
        kBackward = -1,
    };
    virtual StatusWith<std::vector<BSONObj>> findDocuments(OperationContext* opCtx,
                                                           const NamespaceString& nss,
                                                           boost::optional<StringData> indexName,
                                                           ScanDirection scanDirection,
                                                           const BSONObj& startKey,
                                                           BoundInclusion boundInclusion,
                                                           std::size_t limit) = 0;

    /**
     * Deletes at most "limit" documents returned by a collection or index scan on the collection in
     * the requested direction. Returns deleted documents on success.
     * The documents returned will be copied and buffered. No cursors on the underlying collection
     * will be kept open once this function returns.
     * If "indexName" is null, a collection scan is used to locate the document.
     */
    virtual StatusWith<std::vector<BSONObj>> deleteDocuments(OperationContext* opCtx,
                                                             const NamespaceString& nss,
                                                             boost::optional<StringData> indexName,
                                                             ScanDirection scanDirection,
                                                             const BSONObj& startKey,
                                                             BoundInclusion boundInclusion,
                                                             std::size_t limit) = 0;

    /**
     * Finds a singleton document in a collection. Returns 'CollectionIsEmpty' if the collection
     * is empty or 'TooManyMatchingDocuments' if it is not a singleton collection.
     */
    virtual StatusWith<BSONObj> findSingleton(OperationContext* opCtx,
                                              const NamespaceString& nss) = 0;

    /**
     * Updates a singleton document in a collection. Upserts the document if it does not exist. If
     * the document is upserted and no '_id' is provided, one will be generated.
     * If the collection has more than 1 document, the update will only be performed on the first
     * one found.
     * Returns 'NamespaceNotFound' if the collection does not exist. This does not implicitly
     * create the collection so that the caller can create the collection with any collection
     * options they want (ex: capped, temp, collation, etc.).
     */
    virtual Status putSingleton(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const BSONObj& update) = 0;

    /**
     * Updates a singleton document in a collection. Never upsert.
     *
     * If the collection has more than 1 document, the update will only be performed on the first
     * one found.
     * Returns 'NamespaceNotFound' if the collection does not exist. This does not implicitly
     * create the collection so that the caller can create the collection with any collection
     * options they want (ex: capped, temp, collation, etc.).
     */
    virtual Status updateSingleton(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const BSONObj& query,
                                   const BSONObj& update) = 0;

    /**
     * Finds a single document in the collection referenced by the specified _id.
     *
     * Not supported on collections with a default collation.
     */
    virtual StatusWith<BSONObj> findById(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const BSONElement& idKey) = 0;

    /**
     * Deletes a single document in the collection referenced by the specified _id.
     * Returns deleted document on success.
     *
     * Not supported on collections with a default collation.
     */
    virtual StatusWith<BSONObj> deleteById(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const BSONElement& idKey) = 0;

    /**
     * Updates a single document in the collection referenced by the specified _id.
     * The document is located by looking up "idKey" in the id index.
     * "update" represents the replacement document or list of requested modifications to be applied
     * to the document.
     * If the document is not found, a new document will be created with the requested modifications
     * applied.
     */
    virtual Status upsertById(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const BSONElement& idKey,
                              const BSONObj& update) = 0;

    /**
     * Removes all documents that match the "filter" from a collection.
     * "filter" specifies the deletion criteria using query operators. Pass in an empty document to
     * delete all documents in a collection.
     */
    virtual Status deleteByFilter(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const BSONObj& filter) = 0;

    using CollectionSize = uint64_t;
    using CollectionCount = uint64_t;

    /**
     * Returns the sum of the sizes of documents in the collection in bytes.
     */
    virtual StatusWith<CollectionSize> getCollectionSize(OperationContext* opCtx,
                                                         const NamespaceString& nss) = 0;
    /**
     * Returns the number of documents in the collection.
     */
    virtual StatusWith<CollectionCount> getCollectionCount(OperationContext* opCtx,
                                                           const NamespaceString& nss) = 0;

    /**
     * Returns the UUID of the collection specified by nss, if such a UUID exists.
     */
    virtual StatusWith<OptionalCollectionUUID> getCollectionUUID(OperationContext* opCtx,
                                                                 const NamespaceString& nss) = 0;

    /**
     * Adds UUIDs for non-replicated collections. To be called only at the end of initial
     * sync and only if the admin.system.version collection has a UUID.
     */
    virtual Status upgradeUUIDSchemaVersionNonReplicated(OperationContext* opCtx) = 0;

    /**
     * Sets the highest timestamp at which the storage engine is allowed to take a checkpoint.
     * This timestamp can never decrease, and thus should be a timestamp that can never roll back.
     */
    virtual void setStableTimestamp(ServiceContext* serviceCtx, Timestamp snapshotName) = 0;

    /**
     * Tells the storage engine the timestamp of the data at startup. This is necessary because
     * timestamps are not persisted in the storage layer.
     */
    virtual void setInitialDataTimestamp(ServiceContext* serviceCtx, Timestamp snapshotName) = 0;

    /**
     * Reverts the state of all database data to the last stable timestamp.
     *
     * The "local" database is exempt and none of its state should be reverted except for
     * "local.replset.minvalid" and "local.replset.checkpointTimestamp" which should be reverted to
     * the last stable timestamp.
     *
     * The 'stable' timestamp is set by calling StorageInterface::setStableTimestamp.
     */
    virtual Status recoverToStableTimestamp(ServiceContext* serviceCtx) = 0;

    /**
     * Waits for oplog writes to be visible in the oplog.
     * This function is used to ensure tests do not fail due to initial sync receiving an empty
     * batch.
     */
    virtual void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx) = 0;

    /**
     * Returns the all committed timestamp. All transactions with timestamps earlier than the
     * all committed timestamp are committed. Only storage engines that support document level
     * locking must provide an implementation. Other storage engines may provide a no-op
     * implementation.
     */
    virtual Timestamp getAllCommittedTimestamp(ServiceContext* serviceCtx) const = 0;

    /**
     * Returns true if the storage engine supports document level locking.
     */
    virtual bool supportsDocLocking(ServiceContext* serviceCtx) const = 0;
};

}  // namespace repl
}  // namespace mongo
