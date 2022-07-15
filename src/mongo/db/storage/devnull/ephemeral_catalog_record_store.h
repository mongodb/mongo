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

#include <boost/shared_array.hpp>
#include <map>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/storage/capped_callback.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/concurrency/with_lock.h"


namespace mongo {

/**
 * A RecordStore that stores all data in-memory.
 *
 * @param cappedMaxSize - required if isCapped. limit uses dataSize() in this impl.
 */
class EphemeralForTestRecordStore : public RecordStore {
public:
    explicit EphemeralForTestRecordStore(StringData ns,
                                         StringData identName,
                                         std::shared_ptr<void>* dataInOut,
                                         bool isCapped = false,
                                         CappedCallback* cappedCallback = nullptr);

    virtual const char* name() const;

    virtual KeyFormat keyFormat() const {
        return KeyFormat::Long;
    }

    virtual RecordData dataFor(OperationContext* opCtx, const RecordId& loc) const;

    virtual bool findRecord(OperationContext* opCtx, const RecordId& loc, RecordData* rd) const;

    void doDeleteRecord(OperationContext* opCtx, const RecordId& dl) override;

    Status doInsertRecords(OperationContext* opCtx,
                           std::vector<Record>* inOutRecords,
                           const std::vector<Timestamp>& timestamps) override;

    Status doUpdateRecord(OperationContext* opCtx,
                          const RecordId& oldLocation,
                          const char* data,
                          int len) override;

    virtual bool updateWithDamagesSupported() const;

    StatusWith<RecordData> doUpdateWithDamages(OperationContext* opCtx,
                                               const RecordId& loc,
                                               const RecordData& oldRec,
                                               const char* damageSource,
                                               const mutablebson::DamageVector& damages) override;

    virtual void printRecordMetadata(OperationContext* opCtx, const RecordId& recordId) const {}

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward) const final;

    Status doTruncate(OperationContext* opCtx) override;

    void doCappedTruncateAfter(OperationContext* opCtx,
                               const RecordId& end,
                               bool inclusive) override;

    virtual void appendNumericCustomStats(OperationContext* opCtx,
                                          BSONObjBuilder* result,
                                          double scale) const {}

    virtual int64_t storageSize(OperationContext* opCtx,
                                BSONObjBuilder* extraInfo = nullptr,
                                int infoLevel = 0) const;

    virtual long long dataSize(OperationContext* opCtx) const {
        return _data->dataSize;
    }

    virtual long long numRecords(OperationContext* opCtx) const {
        return _data->records.size();
    }

    virtual void updateStatsAfterRepair(OperationContext* opCtx,
                                        long long numRecords,
                                        long long dataSize) {
        stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);
        invariant(_data->records.size() == size_t(numRecords));
        _data->dataSize = dataSize;
    }

protected:
    struct EphemeralForTestRecord {
        EphemeralForTestRecord() : size(0) {}
        EphemeralForTestRecord(int size) : size(size), data(new char[size]) {}

        RecordData toRecordData() const {
            return RecordData(data.get(), size);
        }

        int size;
        boost::shared_array<char> data;
    };

    virtual const EphemeralForTestRecord* recordFor(WithLock, const RecordId& loc) const;
    virtual EphemeralForTestRecord* recordFor(WithLock, const RecordId& loc);
    void waitForAllEarlierOplogWritesToBeVisibleImpl(OperationContext* opCtx) const override {}


public:
    //
    // Not in RecordStore interface
    //

    typedef std::map<RecordId, EphemeralForTestRecord> Records;

    bool isCapped() const {
        return _isCapped;
    }
    void setCappedCallback(CappedCallback* cb) {
        _cappedCallback = cb;
    }

private:
    class RemoveChange;
    class TruncateChange;

    class Cursor;
    class ReverseCursor;

    StatusWith<RecordId> extractAndCheckLocForOplog(WithLock, const char* data, int len) const;

    RecordId allocateLoc(WithLock);
    void deleteRecord(WithLock lk, OperationContext* opCtx, const RecordId& dl);

    const bool _isCapped;
    CappedCallback* _cappedCallback;

    // This is the "persistent" data.
    struct Data {
        Data(StringData ns, bool isOplog)
            : dataSize(0), recordsMutex(), nextId(1), isOplog(isOplog) {}

        int64_t dataSize;
        stdx::recursive_mutex recordsMutex;
        Records records;
        int64_t nextId;
        const bool isOplog;
    };

    Data* const _data;
};

}  // namespace mongo
