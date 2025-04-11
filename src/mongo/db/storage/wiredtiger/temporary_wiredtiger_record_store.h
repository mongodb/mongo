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

#include "mongo/db/storage/wiredtiger/temporary_wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"

namespace mongo {

class RecoveryUnit;
class TemporaryWiredTigerKVEngine;

/**
 * WiredTigerRecordStoreBase implementation for temporary tables. Temporary tables are tables that
 * don't need to be retained after a restart. Use TemporaryWiredTigerKVEngine to create an instance
 * of this class.
 *
 * This class is not thread-safe. A single thread must be interacting with this RecordStore or any
 * cursors created from it at any given time. This class creates its own RecoveryUnit instance and
 * uses it for all operations performed through this class.
 */
class TemporaryWiredTigerRecordStore : public WiredTigerRecordStoreBase {
public:
    struct Params {
        WiredTigerRecordStoreBase::Params baseParams;
    };

    TemporaryWiredTigerRecordStore(TemporaryWiredTigerKVEngine* kvEngine, Params params);

    ~TemporaryWiredTigerRecordStore() override;

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward = true) const override;

private:
    long long dataSize() const override;

    long long numRecords() const override;

    int64_t storageSize(RecoveryUnit&,
                        BSONObjBuilder* extraInfo = nullptr,
                        int infoLevel = 0) const override;

    /**
     * The 'opCtx' param is not used and is allowed to be nullptr.
     */
    RecoveryUnit& getRecoveryUnit(OperationContext* opCtx) const override;

    void _deleteRecord(OperationContext* opCtx, const RecordId&) override;

    // TODO(SERVER-103259): Remove the timestamp param.
    Status _insertRecords(OperationContext* opCtx,
                          std::vector<Record>*,
                          const std::vector<Timestamp>&) override;

    Status _updateRecord(OperationContext* opCtx,
                         const RecordId&,
                         const char* data,
                         int len) override;

    Status _truncate(OperationContext* opCtx) override;

    Status _rangeTruncate(OperationContext* opCtx,
                          const RecordId& minRecordId = RecordId(),
                          const RecordId& maxRecordId = RecordId(),
                          int64_t hintDataSizeIncrement = 0,
                          int64_t hintNumRecordsIncrement = 0) override;

    StatusWith<RecordData> _updateWithDamages(OperationContext* opCtx,
                                              const RecordId& loc,
                                              const RecordData& oldRec,
                                              const char* damageSource,
                                              const DamageVector& damages) override {
        MONGO_UNREACHABLE;
    }

    StatusWith<int64_t> _compact(OperationContext* opCtx, const CompactOptions&) override {
        MONGO_UNREACHABLE;
    }

    int64_t freeStorageSize(RecoveryUnit& ru) const override {
        MONGO_UNREACHABLE;
    }

    bool updateWithDamagesSupported() const override {
        return false;
    }

    bool compactSupported() const override {
        return false;
    }

    void printRecordMetadata(const RecordId&, std::set<Timestamp>*) const override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<RecordCursor> getRandomCursor(OperationContext* opCtx) const override {
        MONGO_UNREACHABLE;
    }

    void validate(RecoveryUnit&,
                  const CollectionValidation::ValidationOptions&,
                  ValidateResults*) override {
        MONGO_UNREACHABLE;
    }

    void appendNumericCustomStats(RecoveryUnit&, BSONObjBuilder*, double scale) const override {
        MONGO_UNREACHABLE;
    }

    void appendAllCustomStats(RecoveryUnit&, BSONObjBuilder*, double scale) const override {
        MONGO_UNREACHABLE;
    }

    RecordId getLargestKey(OperationContext* opCtx) const override {
        MONGO_UNREACHABLE;
    }

    void reserveRecordIds(OperationContext* opCtx,
                          std::vector<RecordId>*,
                          size_t numRecords) override {
        MONGO_UNREACHABLE;
    }

    void updateStatsAfterRepair(long long numRecords, long long dataSize) override {
        MONGO_UNREACHABLE;
    }

    Capped* capped() override {
        MONGO_UNREACHABLE;
    }

    RecordStore::Oplog* oplog() override {
        MONGO_UNREACHABLE;
    }

    /**
     * Adjusts the record count and data size metadata for this record store.
     */
    void _changeNumRecordsAndDataSize(int64_t numRecordDiff, int64_t dataSizeDiff);

    TemporaryWiredTigerKVEngine* _kvEngine{nullptr};
    std::unique_ptr<TemporaryWiredTigerRecoveryUnit> _wtRu;
    WiredTigerSizeStorer::SizeInfo _sizeInfo;
};

/**
 * WiredTigerRecordStoreCursorBase implementation for TemporaryWiredTigerRecordStore. It uses the
 * provided RecoveryUnit instance for all operations performed through this class.
 */
class TemporaryWiredTigerRecordStoreCursor final : public WiredTigerRecordStoreCursorBase {
public:
    TemporaryWiredTigerRecordStoreCursor(OperationContext* opCtx,
                                         const TemporaryWiredTigerRecordStore& rs,
                                         bool forward,
                                         TemporaryWiredTigerRecoveryUnit* wtRu);

protected:
    RecoveryUnit& getRecoveryUnit() const override;

private:
    TemporaryWiredTigerRecoveryUnit* _wtRu{nullptr};
};

}  // namespace mongo
