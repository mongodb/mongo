/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/catalog/throttle_cursor.h"
#include "mongo/db/repl/dbcheck.h"
#include "mongo/db/repl/dbcheck_gen.h"
#include "mongo/db/repl/dbcheck_idl.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * All the information needed to run dbCheck on a single collection.
 */
struct DbCheckCollectionInfo {
    NamespaceString nss;
    UUID uuid;
    BSONObj start;
    BSONObj end;
    int64_t maxCount;
    int64_t maxSize;
    int64_t maxDocsPerBatch;
    int64_t maxBatchTimeMillis;
    WriteConcernOptions writeConcern;
    boost::optional<SecondaryIndexCheckParameters> secondaryIndexCheckParameters;
    DataThrottle dataThrottle;

    // Returns the BSON representation of this object.
    BSONObj toBSON() const;
};

/**
 * For organizing the results of batches for collection-level db check.
 */
struct DbCheckCollectionBatchStats {
    boost::optional<UUID> batchId;
    int64_t nDocs;
    int64_t nBytes;
    BSONObj lastKey;
    std::string md5;
    repl::OpTime time;
    bool logToHealthLog;
    boost::optional<Timestamp> readTimestamp;
};

/**
 * For organizing the results of batches for extra index keys check.
 */
struct DbCheckExtraIndexKeysBatchStats {
    int64_t nKeys;
    int64_t nBytes;
    key_string::Value lastKeyChecked;
    key_string::Value nextKeyToBeChecked;
    bool finishedIndexBatch;
    bool finishedIndexCheck;
    std::string md5;
    repl::OpTime time;
    boost::optional<Timestamp> readTimestamp;
    Date_t deadline;
    // Number of consecutive identical keys at the end of the batch.
    int64_t nConsecutiveIdenticalKeysAtEnd = 1;
};

/**
 * RAII-style class, which logs dbCheck start and stop events in the healthlog and replicates them.
 * The parameter info is boost::none when for a fullDatabaseRun where all collections are not
 * replicated.
 */
// TODO SERVER-79132: Remove boost::optional from _info once dbCheck no longer allows for full
// database run
class DbCheckStartAndStopLogger {
    boost::optional<DbCheckCollectionInfo> _info;

public:
    DbCheckStartAndStopLogger(OperationContext* opCtx, boost::optional<DbCheckCollectionInfo> info);

    ~DbCheckStartAndStopLogger();

private:
    OperationContext* _opCtx;
};

/**
 * A run of dbCheck consists of a series of collections.
 */
using DbCheckRun = std::vector<DbCheckCollectionInfo>;

std::unique_ptr<DbCheckRun> singleCollectionRun(OperationContext* opCtx,
                                                const DatabaseName& dbName,
                                                const DbCheckSingleInvocation& invocation);

std::unique_ptr<DbCheckRun> fullDatabaseRun(OperationContext* opCtx,
                                            const DatabaseName& dbName,
                                            const DbCheckAllInvocation& invocation);

/**
 * Factory function for producing DbCheckRun's from command objects.
 */
std::unique_ptr<DbCheckRun> getRun(OperationContext* opCtx,
                                   const DatabaseName& dbName,
                                   const BSONObj& obj);

/**
 * The BackgroundJob in which dbCheck actually executes on the primary.
 */
class DbCheckJob : public BackgroundJob {
public:
    DbCheckJob(Service* service, std::unique_ptr<DbCheckRun> run)
        : BackgroundJob(true), _service(service), _run(std::move(run)) {}

protected:
    std::string name() const override {
        return "dbCheck";
    }

    void run() override;

private:
    Service* _service;
    std::unique_ptr<DbCheckRun> _run;
};

class DbChecker {
public:
    DbChecker(DbCheckCollectionInfo info) : _info(info){};

    /**
     * Runs dbCheck on the collection specified in the DbCheckCollectionInfo struct.
     */
    void doCollection(OperationContext* opCtx);

private:
    boost::optional<key_string::Value> getExtraIndexKeysCheckFirstKey(OperationContext* opCtx);

    void _extraIndexKeysCheck(OperationContext* opCtx);

    /**
     * Sets up a hasher and hashes one batch for extra index keys check.
     * Returns a non-OK Status if we encountered an error and should abandon extra index keys check.
     */
    Status _hashExtraIndexKeysCheck(OperationContext* opCtx,
                                    const key_string::Value& batchFirst,
                                    const key_string::Value& batchLast,
                                    DbCheckExtraIndexKeysBatchStats* batchStats);

    /**
     * Gets batch bounds for extra index keys check and stores the info in batchStats. Runs
     * reverse lookup if skipLookupForExtraKeys is not set.
     * Returns a non-OK Status if we encountered an error and should abandon extra index keys check.
     */
    Status _getExtraIndexKeysBatchAndRunReverseLookup(OperationContext* opCtx,
                                                      StringData indexName,
                                                      key_string::Value batchFirst,
                                                      DbCheckExtraIndexKeysBatchStats& batchStats);

    /**
     * Acquires a consistent catalog snapshot and iterates through the secondary index in order
     * to get the batch bounds. Runs reverse lookup if skipLookupForExtraKeys is not set.
     *
     * We release the snapshot by exiting the function. This occurs when:
     *   * we have finished the whole extra index keys check,
     *   * we have finished one batch
     *   * The number of keys we've looked at has met or exceeded
     *     dbCheckMaxTotalIndexKeysPerSnapshot
     *   * if we have identical keys at the end of the batch, one of the above conditions is met and
     *     the number of consecutive identical keys we've looked at has met or exceeded
     *     dbCheckMaxConsecutiveIdenticalIndexKeysPerSnapshot
     *
     * Returns a non-OK Status if we encountered an error and should abandon extra index keys check.
     */
    Status _getCatalogSnapshotAndRunReverseLookup(OperationContext* opCtx,
                                                  StringData indexName,
                                                  const key_string::Value& snapshotFirstKey,
                                                  DbCheckExtraIndexKeysBatchStats& batchStats);

    /**
     * Returns if we should end the current catalog snapshot based on meeting snapshot/batch limits.
     * Also updates batchStats accordingly with the next batch's starting key, and whether
     * the batch and/or index check has finished.
     */
    bool _shouldEndCatalogSnapshotOrBatch(
        OperationContext* opCtx,
        const CollectionPtr& collection,
        const StringData& indexName,
        const key_string::Value& keyString,
        const BSONObj& keyStringBson,
        int64_t numKeysInSnapshot,
        const SortedDataIndexAccessMethod* iam,
        const std::unique_ptr<SortedDataInterfaceThrottleCursor>& indexCursor,
        DbCheckExtraIndexKeysBatchStats& batchStats,
        const boost::optional<KeyStringEntry>& nextIndexKey);

    /**
     * Iterates through an index table and fetches the corresponding document for each index entry.
     */
    void _reverseLookup(OperationContext* opCtx,
                        StringData indexName,
                        DbCheckExtraIndexKeysBatchStats& batchStats,
                        const CollectionPtr& collection,
                        const key_string::Value& keyString,
                        const BSONObj& keyStringBson,
                        const SortedDataIndexAccessMethod* iam,
                        const IndexCatalogEntry* indexCatalogEntry,
                        const BSONObj& indexSpec);

    /**
     * Runs the data consistency and missing index key check.
     */
    void _dataConsistencyCheck(OperationContext* opCtx);

    StatusWith<DbCheckCollectionBatchStats> _runBatch(OperationContext* opCtx,
                                                      const BSONObj& first);

    /**
     * Acquire the required locks for dbcheck to run on the given namespace.
     */
    std::unique_ptr<DbCheckAcquisition> _acquireDBCheckLocks(OperationContext* opCtx,
                                                             const NamespaceString& nss);

    bool _shouldLogBatch(DbCheckOplogBatch& batch);

    DbCheckCollectionInfo _info;

    // Cumulative number of batches processed. Can wrap around; it's not guaranteed to be in
    // lockstep with other replica set members.
    unsigned int _batchesProcessed = 0;
};

}  // namespace mongo
