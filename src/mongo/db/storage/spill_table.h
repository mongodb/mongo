/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"

#include <memory>

#include <boost/optional.hpp>

namespace mongo {

class DiskSpaceMonitor;
class StorageEngine;

/**
 * SpillTable provides an interface for interacting with a RecordStore used for spilling
 * intermediate data during query execution or other processing workflows.
 *
 * This class is not thread-safe. A single thread must be interacting with a SpillTable instance or
 * any cursors created from it at any given time. Note that the underlying RecordStore creates its
 * own RecoveryUnit instance and uses it for all operations performed through this class.
 */
class SpillTable {
public:
    class Cursor {
    public:
        Cursor(RecoveryUnit* ru, std::unique_ptr<SeekableRecordCursor> cursor);

        boost::optional<Record> seekExact(const RecordId& id);

        boost::optional<Record> next();

        void detachFromOperationContext();

        void reattachToOperationContext(OperationContext* opCtx);

        void save();

        bool restore(RecoveryUnit& ru);

    private:
        RecoveryUnit* _ru;  // TODO (SERVER-106716): Make this a reference.
        std::unique_ptr<SeekableRecordCursor> _cursor;
    };

    /**
     * Creates a spill table using the given recovery unit and record store.
     *
     * TODO (SERVER-106716): Remove this constructor.
     */
    SpillTable(std::unique_ptr<RecoveryUnit> ru, std::unique_ptr<RecordStore> rs);

    /**
     * Creates a spill table using the given recovery unit and record store. If the available disk
     * space falls below thresholdBytes, writes to the spill table will fail.
     */
    SpillTable(std::unique_ptr<RecoveryUnit> ru,
               std::unique_ptr<RecordStore> rs,
               StorageEngine& storageEngine,
               DiskSpaceMonitor& diskMonitor,
               int64_t thresholdBytes);

    virtual ~SpillTable();

    StringData ident() const;

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
     *
     * TODO (SERVER-106716): Remove the RecoveryUnit parameter.
     */
    int64_t storageSize(RecoveryUnit& ru) const;

    /**
     * Inserts the specified records into the underlying RecordStore by copying the provided record
     * data.
     * When `featureFlagCreateSpillKVEngine` is enabled, this should not be explicitly called in a
     * WriteUnitOfWork.
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
     * 'data' and 'len'.
     * When `featureFlagCreateSpillKVEngine` is enabled, this should not be explicitly called in a
     * WriteUnitOfWork.
     */
    Status updateRecord(OperationContext* opCtx, const RecordId& rid, const char* data, int len);

    /**
     * Deletes the record with id 'rid'.
     * When `featureFlagCreateSpillKVEngine` is enabled, this should not be explicitly called in a
     * WriteUnitOfWork.
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
     * Removes all records.
     * When `featureFlagCreateSpillKVEngine` is enabled, this should not be explicitly called in a
     * WriteUnitOfWork.
     */
    Status truncate(OperationContext* opCtx);

    /**
     * Removes all records in the range [minRecordId, maxRecordId] inclusive of both. The hint*
     * arguments serve as a hint to the record store of how much data will be truncated. This is
     * necessary to avoid reading the data between the two RecordIds in order to update numRecords
     * and dataSize correctly.
     * When `featureFlagCreateSpillKVEngine` is enabled, this should not be explicitly called in a
     * WriteUnitOfWork.
     */
    Status rangeTruncate(OperationContext* opCtx,
                         const RecordId& minRecordId = RecordId(),
                         const RecordId& maxRecordId = RecordId(),
                         int64_t hintDataSizeIncrement = 0,
                         int64_t hintNumRecordsIncrement = 0);

    std::unique_ptr<StorageStats> computeOperationStatisticsSinceLastCall();

protected:
    std::unique_ptr<RecoveryUnit> _ru;
    std::unique_ptr<RecordStore> _rs;
    StorageEngine* _storageEngine{nullptr};  // TODO (SERVER-106716): Make this a reference.

private:
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
