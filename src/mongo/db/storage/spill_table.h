// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/optional.hpp>

namespace mongo {

class DiskSpaceMonitor;

/**
 * SpillTable provides an interface for interacting with a RecordStore used for spilling
 * intermediate data during query execution or other processing workflows.
 *
 * This class is not thread-safe. A single thread must be interacting with a SpillTable instance or
 * any cursors created from it at any given time. Note that the underlying RecordStore creates its
 * own RecoveryUnit instance and uses it for all operations performed through this class.
 */
class [[MONGO_MOD_PUBLIC]] SpillTable final {
public:
    class Cursor {
    public:
        Cursor(RecoveryUnit& ru, std::unique_ptr<SeekableRecordCursor> cursor);

        boost::optional<Record> seekExact(const RecordId& id);

        boost::optional<Record> next();

        void detachFromOperationContext();

        void reattachToOperationContext(OperationContext* opCtx);

        void save();

        bool restore();

    private:
        RecoveryUnit& _ru;
        std::unique_ptr<SeekableRecordCursor> _cursor;
    };

    /**
     * Creates a spill table using the given recovery unit and record store. If the available disk
     * space falls below thresholdBytes, writes to the spill table will fail.
     */
    SpillTable(std::unique_ptr<RecoveryUnit> ru,
               std::unique_ptr<RecordStore> rs,
               StorageEngine& storageEngine,
               DiskSpaceMonitor& diskMonitor,
               int64_t thresholdBytes);

    ~SpillTable();

    std::string_view ident() const;

    /**
     * The dataSize is an approximation of the sum of the sizes (in bytes) of the documents or
     * entries in the underlying RecordStore.
     */
    long long dataSize() const;

    /**
     * Total number of records in the RecordStore.
     */
    long long numRecords() const;

    /**
     * Returns the storage size on disk of the spill table.
     */
    int64_t storageSize() const;

    /**
     * Inserts the specified records into the underlying RecordStore by copying the provided record
     * data. This should not be explicitly called in a WriteUnitOfWork.
     */
    Status insertRecords(OperationContext* opCtx, std::vector<Record>* records);

    /**
     * Finds the record with the specified RecordId in the underlying RecordStore. Returns true iff
     * the record is found.
     *
     * If unowned data is returned, it is valid until the next modification.
     */
    bool findRecord(OperationContext* opCtx, const RecordId& rid, RecordData* out) const;

    /**
     * Updates the record with id 'rid', replacing its contents with those described by
     * 'data' and 'len'. This should not be explicitly called in a WriteUnitOfWork.
     */
    Status updateRecord(OperationContext* opCtx, const RecordId& rid, const char* data, int len);

    /**
     * Deletes the record with id 'rid'. This should not be explicitly called in a WriteUnitOfWork.
     */
    void deleteRecord(OperationContext* opCtx, const RecordId& rid);

    /**
     * Returns a new cursor over the underlying RecordStore.
     *
     * The cursor is logically positioned before the first (or last if !forward) record in the
     * collection so that a record will be returned on the first call to next().
     */
    std::unique_ptr<Cursor> getCursor(OperationContext*, bool forward = true) const;

    /**
     * Removes all records. This should not be explicitly called in a WriteUnitOfWork.
     */
    Status truncate(OperationContext* opCtx);

    /**
     * Removes all records in the range [minRecordId, maxRecordId] inclusive of both. The hint*
     * arguments serve as a hint to the record store of how much data will be truncated. This is
     * necessary to avoid reading the data between the two RecordIds in order to update numRecords
     * and dataSize correctly. This should not be explicitly called in a WriteUnitOfWork.
     */
    Status rangeTruncate(OperationContext* opCtx,
                         const RecordId& minRecordId = RecordId(),
                         const RecordId& maxRecordId = RecordId(),
                         int64_t hintDataSizeIncrement = 0,
                         int64_t hintNumRecordsIncrement = 0);

    std::unique_ptr<StorageStats> computeOperationStatisticsSinceLastCall();

    RecordStore* getRecordStore_forTest();

private:
    std::unique_ptr<RecoveryUnit> _ru;
    std::unique_ptr<RecordStore> _rs;
    StorageEngine& _storageEngine;

    Status _checkDiskSpace() const;

    class DiskState {
    public:
        DiskState(DiskSpaceMonitor& monitor, int64_t thresholdBytes);

        ~DiskState();

        bool full() const;

    private:
        DiskSpaceMonitor& _monitor;
        int64_t _actionId = -1;
        Atomic<bool> _full = false;
    };
    boost::optional<DiskState> _diskState;
};

}  // namespace mongo
