/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/repl/collection_bulk_loader.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace repl {

struct TimestampedBSONObj {
    BSONObj obj;
    Timestamp timestamp;
};

/**
 * Storage interface used by the replication system to interact with storage.
 * This interface provides separation of concerns and a place for mocking out test
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
    StorageInterface(const StorageInterface&) = delete;
    StorageInterface& operator=(const StorageInterface&) = delete;

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
        BSONObj idIndexSpec,
        const std::vector<BSONObj>& secondaryIndexSpecs) = 0;

    /**
     * Inserts a document with a timestamp into a collection.
     *
     * NOTE: If the collection doesn't exist, it will not be created, and instead
     * an error is returned.
     */
    virtual Status insertDocument(OperationContext* opCtx,
                                  const NamespaceStringOrUUID& nsOrUUID,
                                  const TimestampedBSONObj& doc,
                                  long long term) = 0;

    /**
     * Inserts the given documents, with associated timestamps and statement id's, into the
     * collection.
     * It is an error to call this function with an empty set of documents.
     *
     * NOTE: We have some limitations with this function if the caller plans to use it for
     * replicated collection writes and 'docs' size greater than 1.
     * 1) It will violate multi-timestamp constraints (See SERVER-48771).
     * 2) It doesn't have batch size throttling logic so there are possibilities that we might cause
     * stress on storage engine by trying to insert a big chunk of data in a single WUOW.
     *    - Another side effect of writing a big chunk is that writers will hold RSTL lock for a
     * long time, causing state transition (like step down) to get blocked.
     *
     * So, it's recommended to use write_ops_exec::performInserts() for replicated collection
     * writes.
     */
    virtual Status insertDocuments(OperationContext* opCtx,
                                   const NamespaceStringOrUUID& nsOrUUID,
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
    virtual StatusWith<size_t> getOplogMaxSize(OperationContext* opCtx) = 0;

    /**
     * Creates a collection.
     */
    virtual Status createCollection(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const CollectionOptions& options,
                                    bool createIdIndex = true,
                                    const BSONObj& idIndexSpec = BSONObj()) = 0;

    /**
     * Creates all the specified non-_id indexes on a given collection, which must be empty.
     * Note: This function assumes the give collection is a committed collection, so it takes
     * an exclusive collection lock on that collection.
     */
    virtual Status createIndexesOnEmptyCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const std::vector<BSONObj>& secondaryIndexSpecs) = 0;

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
     * Sets the given index on the given namespace as multikey with the given paths. Does the
     * write at the provided timestamp.
     */
    virtual Status setIndexIsMultikey(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const UUID& collectionUUID,
                                      const std::string& indexName,
                                      const KeyStringSet& multikeyMetadataKeys,
                                      const MultikeyPaths& paths,
                                      Timestamp ts) = 0;
    /**
     * Drops all databases except "local".
     */
    virtual Status dropReplicatedDatabases(OperationContext* opCtx) = 0;

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
     * one found. The upsert is performed at the given timestamp.
     * Returns 'NamespaceNotFound' if the collection does not exist. This does not implicitly
     * create the collection so that the caller can create the collection with any collection
     * options they want (ex: capped, temp, collation, etc.).
     */
    virtual Status putSingleton(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const TimestampedBSONObj& update) = 0;

    /**
     * Updates a singleton document in a collection. Never upsert.
     *
     * If the collection has more than 1 document, the update will only be performed on the first
     * one found. The update is performed at the given timestamp.
     * Returns 'NamespaceNotFound' if the collection does not exist. This does not implicitly
     * create the collection so that the caller can create the collection with any collection
     * options they want (ex: capped, temp, collation, etc.).
     */
    virtual Status updateSingleton(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const BSONObj& query,
                                   const TimestampedBSONObj& update) = 0;

    /**
     * Finds a single document in the collection referenced by the specified _id.
     *
     * Not supported on collections with a default collation.
     */
    virtual StatusWith<BSONObj> findById(OperationContext* opCtx,
                                         const NamespaceStringOrUUID& nsOrUUID,
                                         const BSONElement& idKey) = 0;

    /**
     * Deletes a single document in the collection referenced by the specified _id.
     * Returns deleted document on success.
     *
     * Not supported on collections with a default collation.
     */
    virtual StatusWith<BSONObj> deleteById(OperationContext* opCtx,
                                           const NamespaceStringOrUUID& nsOrUUID,
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
                              const NamespaceStringOrUUID& nsOrUUID,
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

    /**
     * Searches for an oplog optime with a timestamp <= 'timestamp'. Returns boost::none if no
     * matches are found.
     */
    virtual boost::optional<OpTimeAndWallTime> findOplogOpTimeLessThanOrEqualToTimestamp(
        OperationContext* opCtx, const CollectionPtr& oplog, const Timestamp& timestamp) = 0;

    /**
     * Calls findOplogOpTimeLessThanOrEqualToTimestamp with endless WriteConflictException retries.
     * Other errors get thrown. Concurrent oplog reads with the validate cmd on the same collection
     * may throw WCEs. Obeys opCtx interrupts.
     *
     * Call this function instead of findOplogOpTimeLessThanOrEqualToTimestamp if the caller cannot
     * fail, say for correctness.
     */
    virtual boost::optional<OpTimeAndWallTime> findOplogOpTimeLessThanOrEqualToTimestampRetryOnWCE(
        OperationContext* opCtx, const CollectionPtr& oplog, const Timestamp& timestamp) = 0;

    /**
     * Fetches the oldest oplog entry's timestamp.
     */
    virtual Timestamp getEarliestOplogTimestamp(OperationContext* opCtx) = 0;

    /**
     * Fetches the latest oplog entry's timestamp. Bypasses the oplog visibility rules.
     */
    virtual Timestamp getLatestOplogTimestamp(OperationContext* opCtx) = 0;

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
    virtual StatusWith<CollectionCount> getCollectionCount(
        OperationContext* opCtx, const NamespaceStringOrUUID& nsOrUUID) = 0;

    /**
     * Sets the number of documents in the collection. This function does NOT also update the
     * data size of the collection.
     */
    virtual Status setCollectionCount(OperationContext* opCtx,
                                      const NamespaceStringOrUUID& nsOrUUID,
                                      long long newCount) = 0;

    /**
     * Returns the UUID of the collection specified by nss, if such a UUID exists.
     */
    virtual StatusWith<UUID> getCollectionUUID(OperationContext* opCtx,
                                               const NamespaceString& nss) = 0;

    /**
     * Sets the highest timestamp at which the storage engine is allowed to take a checkpoint. This
     * timestamp must not decrease unless force=true is set, in which case we force the stable
     * timestamp, the oldest timestamp, and the commit timestamp backward. Additionally when
     * force=true is set, the all durable timestamp will be set to the stable timestamp.
     */
    virtual void setStableTimestamp(ServiceContext* serviceCtx,
                                    Timestamp snapshotName,
                                    bool force = false) = 0;

    /**
     * Tells the storage engine the timestamp of the data at startup. This is necessary because
     * timestamps are not persisted in the storage layer.
     */
    virtual void setInitialDataTimestamp(ServiceContext* serviceCtx, Timestamp snapshotName) = 0;

    /**
     * Reverts the state of all database data to the last stable timestamp.
     *
     * The "local" database is exempt and none of its state should be reverted except for
     * "local.replset.minvalid" which should be reverted to the last stable timestamp.
     *
     * The 'stable' timestamp is set by calling StorageInterface::setStableTimestamp.
     *
     * Returns the stable timestamp to which it reverted the data.
     */
    virtual Timestamp recoverToStableTimestamp(OperationContext* opCtx) = 0;

    /**
     * Returns whether the storage engine supports "recover to stable timestamp".
     */
    virtual bool supportsRecoverToStableTimestamp(ServiceContext* serviceCtx) const = 0;

    /**
     * Returns whether the storage engine can provide a recovery timestamp.
     */
    virtual bool supportsRecoveryTimestamp(ServiceContext* serviceCtx) const = 0;

    /**
     * Responsible for initializing independent processes for replication that manage
     * and interact with the storage layer.
     *
     * Initializes the OplogCapMaintainerThread to control deletion of oplog truncate markers.
     */
    virtual void initializeStorageControlsForReplication(ServiceContext* serviceCtx) const = 0;

    /**
     * Returns the stable timestamp that the storage engine recovered to on startup. If the
     * recovery point was not stable, returns "none".
     */
    virtual boost::optional<Timestamp> getRecoveryTimestamp(ServiceContext* serviceCtx) const = 0;

    /**
     * Waits for oplog writes to be visible in the oplog.
     * This function is used to ensure tests do not fail due to initial sync receiving an empty
     * batch.
     *
     * primaryOnly: If this node is not primary, do nothing.
     */
    virtual void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx,
                                                         bool primaryOnly = false) = 0;

    /**
     * Returns the all_durable timestamp. All transactions with timestamps earlier than the
     * all_durable timestamp are committed.
     *
     * The all_durable timestamp only includes non-prepared transactions that have been given a
     * commit_timestamp and prepared transactions that have been given a durable_timestamp.
     * Previously, the deprecated all_committed timestamp would also include prepared transactions
     * that were prepared but not committed which could make the stable timestamp briefly jump back.
     */
    virtual Timestamp getAllDurableTimestamp(ServiceContext* serviceCtx) const = 0;

    /**
     * Registers a timestamp with the storage engine so that it can enforce oplog visiblity rules.
     * orderedCommit - specifies whether the timestamp provided is ordered w.r.t. commits; that is,
     * all commits with older timestamps have already occurred, and any commits with newer
     * timestamps have not yet occurred.
     */
    virtual void oplogDiskLocRegister(OperationContext* opCtx,
                                      const Timestamp& ts,
                                      bool orderedCommit) = 0;

    /**
     * Returns a timestamp that is guaranteed to exist on storage engine recovery to a stable
     * timestamp. This indicates when the storage engine can safely rollback to stable; and for
     * durable engines, it is also the guaranteed minimum stable recovery point on server restart
     * after crash or shutdown.
     *
     * Returns `Timestamp::min()` if no stable recovery timestamp has yet been established.
     * Replication recoverable rollback may not succeed before establishment, and restart will
     * require resync. Returns boost::none if `supportsRecoverToStableTimestamp` returns false.
     */
    virtual boost::optional<Timestamp> getLastStableRecoveryTimestamp(
        ServiceContext* serviceCtx) const = 0;

    /**
     * Returns the read timestamp of the recovery unit of the given operation context.
     */
    virtual Timestamp getPointInTimeReadTimestamp(OperationContext* opCtx) const = 0;

    /**
     * Prevents oplog history at 'pinnedTimestamp' and later from being truncated. Setting
     * Timestamp::max() effectively nullifies the pin because no oplog truncation will be stopped by
     * it.
     */
    virtual void setPinnedOplogTimestamp(OperationContext* opCtx,
                                         const Timestamp& pinnedTimestamp) const = 0;
};

}  // namespace repl
}  // namespace mongo
