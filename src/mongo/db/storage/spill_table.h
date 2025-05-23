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

#include "mongo/db/storage/record_store.h"

namespace mongo {

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
    explicit SpillTable(std::unique_ptr<RecordStore> rs) : _rs(std::move(rs)) {}

    virtual ~SpillTable() {}

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
    int64_t storageSize(RecoveryUnit& ru) const;

    /**
     * Inserts the specified records into the underlying RecordStore by copying the provided record
     * data.
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
     */
    Status updateRecord(OperationContext* opCtx, const RecordId& rid, const char* data, int len);

    /**
     * Deletes the record with id 'rid'.
     */
    void deleteRecord(OperationContext* opCtx, const RecordId& rid);

    /**
     * Returns a new cursor over the underlying RecordStore.
     *
     * The cursor is logically positioned before the first (or last if !forward) record in the
     * collection so that a record will be returned on the first call to next().
     */
    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext*, bool forward = true) const;

    /**
     * Removes all records.
     */
    Status truncate(OperationContext* opCtx);

    /**
     * Removes all records in the range [minRecordId, maxRecordId] inclusive of both. The hint*
     * arguments serve as a hint to the record store of how much data will be truncated. This is
     * necessary to avoid reading the data between the two RecordIds in order to update numRecords
     * and dataSize correctly.
     */
    Status rangeTruncate(OperationContext* opCtx,
                         const RecordId& minRecordId = RecordId(),
                         const RecordId& maxRecordId = RecordId(),
                         int64_t hintDataSizeIncrement = 0,
                         int64_t hintNumRecordsIncrement = 0);

protected:
    std::unique_ptr<RecordStore> _rs;
};

}  // namespace mongo
