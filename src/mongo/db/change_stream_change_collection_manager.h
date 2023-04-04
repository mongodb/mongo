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

    // Current maximum record id present in the change collection.
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
     * Creates a change collection for the specified tenant, if it doesn't exist.
     */
    void createChangeCollection(OperationContext* opCtx, const TenantId& tenantId);

    /**
     * Deletes the change collection for the specified tenant, if it already exist.
     */
    void dropChangeCollection(OperationContext* opCtx, const TenantId& tenantId);

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
     */
    void insertDocumentsToChangeCollection(OperationContext* opCtx,
                                           const std::vector<Record>& oplogRecords,
                                           const std::vector<Timestamp>& oplogTimestamps);

    class ChangeCollectionsWriterInternal;

    /**
     * Change Collection Writer. After acquiring ChangeCollectionsWriter the user should trigger
     * acquisition of the locks by calling 'acquireLocks()' before the first write in the Write Unit
     * of Work. Then the write of documents to change collections can be triggered by calling
     * 'write()'.
     */
    class ChangeCollectionsWriter {
        friend class ChangeStreamChangeCollectionManager;

        /**
         * Constructs a writer from a range ['beginOplogEntries', 'endOplogEntries') of oplog
         * entries.
         */
        ChangeCollectionsWriter(OperationContext* opCtx,
                                std::vector<InsertStatement>::const_iterator beginOplogEntries,
                                std::vector<InsertStatement>::const_iterator endOplogEntries,
                                OpDebug* opDebug);

    public:
        ChangeCollectionsWriter(ChangeCollectionsWriter&&);
        ChangeCollectionsWriter& operator=(ChangeCollectionsWriter&&);

        /**
         * Acquires locks needed to write documents to change collections.
         */
        void acquireLocks();

        /**
         * Writes documents to change collections.
         */
        Status write();

        ~ChangeCollectionsWriter();

    private:
        std::unique_ptr<ChangeCollectionsWriterInternal> _writer;
    };

    /**
     * Returns a change collection writer that can insert change collection entries into respective
     * change collections. The entries are constructed from a range ['beginOplogEntries',
     * 'endOplogEntries') of oplog entries.
     */
    ChangeCollectionsWriter createChangeCollectionsWriter(
        OperationContext* opCtx,
        std::vector<InsertStatement>::const_iterator beginOplogEntries,
        std::vector<InsertStatement>::const_iterator endOplogEntries,
        OpDebug* opDebug);

    PurgingJobStats& getPurgingJobStats() {
        return _purgingJobStats;
    }

    /**
     * Scans the provided change collection and returns its metadata that will be used by the
     * purging job to perform deletion on it. The method returns 'boost::none' if the collection is
     * empty.
     */
    static boost::optional<ChangeCollectionPurgingJobMetadata>
    getChangeCollectionPurgingJobMetadata(OperationContext* opCtx,
                                          const CollectionPtr* changeCollection);

    /** Removes documents from a change collection whose wall time is less than the
     * 'expirationTime'. Returns the number of documents deleted. The 'maxRecordIdBound' is the
     * maximum record id bound that will not be included in the collection scan.
     */
    static size_t removeExpiredChangeCollectionsDocuments(OperationContext* opCtx,
                                                          const CollectionPtr* changeCollection,
                                                          RecordIdBound maxRecordIdBound,
                                                          Date_t expirationTime);

private:
    // Change collections purging job stats.
    PurgingJobStats _purgingJobStats;
};
}  // namespace mongo
