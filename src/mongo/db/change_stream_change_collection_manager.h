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

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"

namespace mongo {

/**
 * Metadata associated with a particular change collection that is used by the purging job.
 */
struct ChangeCollectionPurgingJobMetadata {
    // The wall time in milliseconds of the first document of the change collection.
    long long firstDocWallTimeMillis;

    // The maximum record id beyond which the change collection documents will be not deleted.
    RecordIdBound maxRecordIdBound;
};

/**
 * Manages the creation, deletion and insertion lifecycle of the change collection.
 */
class ChangeStreamChangeCollectionManager {
public:
    /**
     * Statistics of the change collection purging job.
     */
    struct PurgingJobStats {
        /**
         * Total number of deletion passes completed by the purging job.
         */
        AtomicWord<long long> totalPass;

        /**
         * Cumulative number of change collections documents deleted by the purging job.
         */
        AtomicWord<long long> docsDeleted;

        /**
         * Cumulative size in bytes of all deleted documents from all change collections by the
         * purging job.
         */
        AtomicWord<long long> bytesDeleted;

        /**
         * Cumulative number of change collections scanned by the purging job.
         */
        AtomicWord<long long> scannedCollections;

        /**
         * Cumulative number of milliseconds elapsed since the first pass by the purging job.
         */
        AtomicWord<long long> timeElapsedMillis;

        /**
         * Maximum wall time in milliseconds from the first document of each change collection.
         */
        AtomicWord<long long> maxStartWallTimeMillis;

        /**
         * Serializes the purging job statistics to the BSON object.
         */
        BSONObj toBSON() const;
    };

    explicit ChangeStreamChangeCollectionManager(ServiceContext* service) {}

    ~ChangeStreamChangeCollectionManager() = default;

    /**
     * Creates an instance of the class using the service-context.
     */
    static void create(ServiceContext* service);

    /**
     * Gets the instance of the class using the service context.
     */
    static ChangeStreamChangeCollectionManager& get(ServiceContext* service);

    /**
     * Gets the instance of the class using the operation context.
     */
    static ChangeStreamChangeCollectionManager& get(OperationContext* opCtx);

    /**
     * Returns true if the server is configured such that change collections can be used to record
     * oplog entries; ie, we are running in a Serverless context. Returns false otherwise.
     */
    static bool isChangeCollectionsModeActive();

    /**
     * Returns true if the change collection is present for the specified tenant, false otherwise.
     */
    bool hasChangeCollection(OperationContext* opCtx, boost::optional<TenantId> tenantId) const;

    /**
     * Returns true if the change stream is enabled for the provided tenant, false otherwise.
     */
    bool isChangeStreamEnabled(OperationContext* opCtx, boost::optional<TenantId> tenantId) const;

    /**
     * Creates a change collection for the specified tenant, if it doesn't exist.
     *
     * TODO: SERVER-65950 make tenantId field mandatory.
     */
    void createChangeCollection(OperationContext* opCtx, boost::optional<TenantId> tenantId);

    /**
     * Deletes the change collection for the specified tenant, if it already exist.
     *
     * TODO: SERVER-65950 make tenantId field mandatory.
     */
    void dropChangeCollection(OperationContext* opCtx, boost::optional<TenantId> tenantId);

    /**
     * Inserts documents to change collections. The parameter 'oplogRecords' is a vector of oplog
     * records and the parameter 'oplogTimestamps' is a vector for respective timestamp for each
     * oplog record.
     *
     * The method fetches the tenant-id from the oplog entry, performs necessary modification to the
     * document and then write to the tenant's change collection at the specified oplog timestamp.
     *
     * Failure in insertion to any change collection will result in a fatal exception and will bring
     * down the node.
     *
     * TODO: SERVER-65950 make tenantId field mandatory.
     */
    void insertDocumentsToChangeCollection(OperationContext* opCtx,
                                           const std::vector<Record>& oplogRecords,
                                           const std::vector<Timestamp>& oplogTimestamps);


    /**
     * Performs a range inserts on respective change collections using the oplog entries as
     * specified by 'beginOplogEntries' and 'endOplogEntries'.
     *
     * Bails out if a failure is encountered in inserting documents to a particular change
     * collection.
     */
    Status insertDocumentsToChangeCollection(
        OperationContext* opCtx,
        std::vector<InsertStatement>::const_iterator beginOplogEntries,
        std::vector<InsertStatement>::const_iterator endOplogEntries,
        bool isGlobalIXLockAcquired,
        OpDebug* opDebug);

    PurgingJobStats& getPurgingJobStats() {
        return _purgingJobStats;
    }

    /**
     * Forward-scans the given change collection to return the wall time of the first document as
     * well as recordId of the last, non-terminal document having the wall time less than the
     * 'expirationTime'. Returns 'boost::none' if the collection is empty, or there are no expired
     * documents, or the collection contains a single expired document.
     */

    /**
     * Forward scans the provided change collection and returns its metadata that will be used by
     * the purging job to perform deletion on it. The method returns 'boost::none' if either the
     * collection is empty, or there are no expired documents, or the collection contains a single
     * expired document.
     */
    static boost::optional<ChangeCollectionPurgingJobMetadata>
    getChangeCollectionPurgingJobMetadata(OperationContext* opCtx,
                                          const CollectionPtr* changeCollection,
                                          const Date_t& expirationTime);

    /**
     * Removes expired documents from the change collection for the provided 'tenantId'. A document
     * whose retention time is less than the 'expirationTime' is deleted.
     * Returns wall time of the first document as well as number of documents deleted.
     */
    static size_t removeExpiredChangeCollectionsDocuments(OperationContext* opCtx,
                                                          const CollectionPtr* changeCollection,
                                                          const RecordIdBound& maxRecordIdBound);

private:
    // Change collections purging job stats.
    PurgingJobStats _purgingJobStats;
};
}  // namespace mongo
