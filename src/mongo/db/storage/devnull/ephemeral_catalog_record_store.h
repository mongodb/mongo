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

#include <boost/optional/optional.hpp>
#include <boost/shared_array.hpp>
#include <boost/smart_ptr/shared_array.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/damage_vector.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * A RecordStore that stores all data in-memory.
 *
 * @param cappedMaxSize - required if isCapped. limit uses dataSize() in this impl.
 */
class EphemeralForTestRecordStore : public RecordStore {
public:
    explicit EphemeralForTestRecordStore(boost::optional<UUID> uuid,
                                         StringData identName,
                                         std::shared_ptr<void>* dataInOut,
                                         bool isCapped = false,
                                         bool isOplog = false);

    const char* name() const override;

    boost::optional<UUID> uuid() const override;

    bool isTemp() const override;

    std::shared_ptr<Ident> getSharedIdent() const override;

    const std::string& getIdent() const override;

    void setIdent(std::shared_ptr<Ident>) override;

    KeyFormat keyFormat() const override {
        return KeyFormat::Long;
    }

    RecordData dataFor(OperationContext* opCtx, const RecordId& loc) const override;

    bool findRecord(OperationContext* opCtx, const RecordId& loc, RecordData* rd) const override;

    void deleteRecord(OperationContext*, const RecordId&) override;

    Status insertRecords(OperationContext*,
                         std::vector<Record>*,
                         const std::vector<Timestamp>&) override;

    StatusWith<RecordId> insertRecord(OperationContext*,
                                      const char* data,
                                      int len,
                                      Timestamp) override;

    StatusWith<RecordId> insertRecord(
        OperationContext*, const RecordId&, const char* data, int len, Timestamp) override;

    Status updateRecord(OperationContext*, const RecordId&, const char* data, int len) override;

    bool updateWithDamagesSupported() const override;

    StatusWith<RecordData> updateWithDamages(OperationContext*,
                                             const RecordId& loc,
                                             const RecordData& oldRec,
                                             const char* damageSource,
                                             const DamageVector& damages) override;

    void printRecordMetadata(const RecordId& recordId,
                             std::set<Timestamp>* recordTimestamps) const override {}

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward) const final;

    std::unique_ptr<RecordCursor> getRandomCursor(OperationContext*) const override;

    Status truncate(OperationContext*) override;
    Status rangeTruncate(OperationContext*,
                         const RecordId& minRecordId,
                         const RecordId& maxRecordId,
                         int64_t hintDataSizeDiff,
                         int64_t hintNumRecordsDiff) override;

    bool compactSupported() const override;

    StatusWith<int64_t> compact(OperationContext*, const CompactOptions&) override;

    void validate(RecoveryUnit&,
                  const CollectionValidation::ValidationOptions&,
                  ValidateResults*) override;

    void appendNumericCustomStats(RecoveryUnit& ru,
                                  BSONObjBuilder* result,
                                  double scale) const override {}

    void appendAllCustomStats(RecoveryUnit&, BSONObjBuilder*, double scale) const override {}

    int64_t storageSize(RecoveryUnit&,
                        BSONObjBuilder* extraInfo = nullptr,
                        int infoLevel = 0) const override;

    int64_t freeStorageSize(RecoveryUnit&) const override;

    long long dataSize() const override {
        return _data->dataSize;
    }

    long long numRecords() const override {
        return _data->records.size();
    }

    void updateStatsAfterRepair(long long numRecords, long long dataSize) override {
        stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);
        invariant(_data->records.size() == size_t(numRecords));
        _data->dataSize = dataSize;
    }

    RecordId getLargestKey(OperationContext* opCtx) const final {
        stdx::lock_guard<stdx::recursive_mutex> lock(_data->recordsMutex);
        return RecordId(_data->nextId - 1);
    }

    void reserveRecordIds(OperationContext* opCtx,
                          std::vector<RecordId>* out,
                          size_t nRecords) final{};

    RecordStore::Capped* capped() override {
        return nullptr;
    }

    RecordStore::Oplog* oplog() override {
        return nullptr;
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

public:
    //
    // Not in RecordStore interface
    //

    typedef std::map<RecordId, EphemeralForTestRecord> Records;

    bool isCapped() const {
        return _isCapped;
    }

private:
    class RemoveChange;
    class TruncateChange;

    class Cursor;
    class ReverseCursor;

    StatusWith<RecordId> extractAndCheckLocForOplog(WithLock, const char* data, int len) const;

    RecordId allocateLoc(WithLock);

    boost::optional<UUID> _uuid;
    std::shared_ptr<Ident> _ident;
    const bool _isCapped;

    // This is the "persistent" data.
    struct Data {
        explicit Data(bool isOplog) : dataSize(0), recordsMutex(), nextId(1), isOplog(isOplog) {}

        int64_t dataSize;
        stdx::recursive_mutex recordsMutex;
        Records records;
        int64_t nextId;
        const bool isOplog;
    };

    Data* const _data;
};

}  // namespace mongo
