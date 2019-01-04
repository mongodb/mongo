
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

#include <memory>
#include <string>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/mobile/mobile_sqlite_statement.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

/**
 * A RecordStore that stores all data in SQLite.
 */
class MobileRecordStore : public RecordStore {
public:
    explicit MobileRecordStore(OperationContext* opCtx,
                               StringData ns,
                               const std::string& path,
                               const std::string& ident,
                               const CollectionOptions& options);

    const char* name() const override;

    const std::string& getIdent() const override;

    bool findRecord(OperationContext* opCtx, const RecordId& recId, RecordData* rd) const override;

    void deleteRecord(OperationContext* opCtx, const RecordId& dl) override;

    Status insertRecords(OperationContext* opCtx,
                         std::vector<Record>* inOutRecords,
                         const std::vector<Timestamp>& timestamps) override;

    Status insertRecordsWithDocWriter(OperationContext* opCtx,
                                      const DocWriter* const* docs,
                                      const Timestamp* timestamps,
                                      size_t nDocs,
                                      RecordId* idsOut) override;

    Status updateRecord(OperationContext* opCtx,
                        const RecordId& oldLocation,
                        const char* data,
                        int len) override;

    bool updateWithDamagesSupported() const override;

    StatusWith<RecordData> updateWithDamages(OperationContext* opCtx,
                                             const RecordId& recId,
                                             const RecordData& oldRec,
                                             const char* damageSource,
                                             const mutablebson::DamageVector& damages) override;

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward) const override;

    Status truncate(OperationContext* opCtx) override;

    void cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) override {
        // Capped Collections are not supported, do nothing
    }

    bool compactSupported() const override {
        return false;
    }

    bool compactsInPlace() const override {
        return false;
    }

    Status validate(OperationContext* opCtx,
                    ValidateCmdLevel level,
                    ValidateAdaptor* adaptor,
                    ValidateResults* results,
                    BSONObjBuilder* output) override;

    void appendCustomStats(OperationContext* opCtx,
                           BSONObjBuilder* result,
                           double scale) const override {
        // No custom stats to add
    }

    Status touch(OperationContext* opCtx, BSONObjBuilder* output) const override;

    int64_t storageSize(OperationContext* opCtx,
                        BSONObjBuilder* extraInfo = NULL,
                        int infoLevel = 0) const override;

    long long dataSize(OperationContext* opCtx) const override;

    long long numRecords(OperationContext* opCtx) const override;

    boost::optional<RecordId> oplogStartHack(OperationContext* opCtx,
                                             const RecordId& startingPosition) const override;

    void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx) const override {}

    void updateStatsAfterRepair(OperationContext* opCtx,
                                long long numRecords,
                                long long dataSize) override {}

    bool isCapped() const override {
        // Capped Collections are not supported
        return false;
    }

    void setCappedCallback(CappedCallback* cb) override {
        // Capped Collections are not supported, do nothing
    }

    // Not in record store API.

    /**
     * Creates a new record store inside SQLite.
     * Transational semantics are handled by the caller.
     */
    static void create(OperationContext* opCtx, const std::string& ident);

private:
    class InsertChange;
    class RemoveChange;
    class TruncateChange;

    class NumRecsChange;
    class DataSizeChange;

    class Cursor;

    RecordId _nextId();

    const std::string _path;
    const std::string _ident;

    AtomicWord<long long> _nextIdNum;

    /**
     * Fetches the number of records from the database. _numRecsMutex should be locked before this
     * is called.
     */
    void _initNumRecsIfNeeded_inlock(OperationContext* opCtx) const;

    /**
     * Updates _numRecords. This must be called before the actual change is made to the database.
     */
    void _changeNumRecs(OperationContext* opCtx, int64_t diff);

    /**
     * Resets _numRecords to the new value. Returns true if _numRecs was reset; returns false
     * otherwise.
     */
    bool _resetNumRecsIfNeeded(OperationContext* opCtx, int64_t newNumRecs);

    mutable int64_t _numRecs;
    mutable stdx::mutex _numRecsMutex;
    mutable bool _isNumRecsInitialized = false;

    /**
     * Fetches the data size from the database. _dataSizeMutex should be locked before this is
     * called.
     */
    void _initDataSizeIfNeeded_inlock(OperationContext* opCtx) const;

    /**
     * Updates _dataSize. This must be called before the actual change is made to the database.
     */
    void _changeDataSize(OperationContext* opCtx, int64_t diff);

    /**
     * Resets _dataSize to the new value. Returns true if _dataSize was reset; returns false
     * otherwise.
     */
    bool _resetDataSizeIfNeeded(OperationContext* opCtx, int64_t newDataSize);

    mutable int64_t _dataSize;
    mutable stdx::mutex _dataSizeMutex;
    mutable bool _isDataSizeInitialized = false;
};

}  // namespace mongo
