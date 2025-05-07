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

#include "mongo/db/storage/wiredtiger/spill_record_store.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/spill_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

SpillRecordStore::SpillRecordStore(SpillKVEngine* kvEngine, Params params)
    : WiredTigerRecordStoreBase(std::move(params.baseParams)),
      _kvEngine(kvEngine),
      _wtRu(std::make_unique<SpillRecoveryUnit>(&kvEngine->getConnection())),
      _sizeInfo(0, 0) {}

SpillRecordStore::~SpillRecordStore() {
    // TODO(SERVER-103273): Truncate and drop the table here.
    LOGV2_DEBUG(10158010,
                1,
                "~SpillRecordStore for temporary ident: {getIdent}",
                "getIdent"_attr = getIdent());
}

std::unique_ptr<SeekableRecordCursor> SpillRecordStore::getCursor(OperationContext* opCtx,
                                                                  bool forward) const {
    return std::make_unique<SpillRecordStoreCursor>(opCtx, *this, forward, _wtRu.get());
}

long long SpillRecordStore::dataSize() const {
    auto dataSize = _sizeInfo.dataSize.load();
    return dataSize > 0 ? dataSize : 0;
}

long long SpillRecordStore::numRecords() const {
    auto numRecords = _sizeInfo.numRecords.load();
    return numRecords > 0 ? numRecords : 0;
}

int64_t SpillRecordStore::storageSize(RecoveryUnit& ru,
                                      BSONObjBuilder* extraInfo,
                                      int infoLevel) const {
    auto& wtRu = SpillRecoveryUnit::get(getRecoveryUnit(nullptr));
    WiredTigerSession* session = wtRu.getSessionNoTxn();
    auto result = WiredTigerUtil::getStatisticsValue(
        *session, "statistics:" + getURI(), "statistics=(size)", WT_STAT_DSRC_BLOCK_SIZE);
    uassertStatusOK(result.getStatus());
    return result.getValue();
}

RecoveryUnit& SpillRecordStore::getRecoveryUnit(OperationContext* opCtx) const {
    return *_wtRu;
}

void SpillRecordStore::_deleteRecord(OperationContext* opCtx, const RecordId& id) {
    auto& wtRu = SpillRecoveryUnit::get(getRecoveryUnit(nullptr));
    OpStats opStats{};
    wtDeleteRecord(opCtx, wtRu, id, opStats);
    _changeNumRecordsAndDataSize(-1, -opStats.oldValueLength);
}

Status SpillRecordStore::_insertRecords(OperationContext* opCtx,
                                        std::vector<Record>* records,
                                        const std::vector<Timestamp>&) {
    auto& wtRu = SpillRecoveryUnit::get(getRecoveryUnit(nullptr));
    auto cursorParams = getWiredTigerCursorParams(wtRu, _tableId, _overwrite);
    WiredTigerCursor curwrap(std::move(cursorParams), _uri, *wtRu.getSession());
    WT_CURSOR* c = curwrap.get();
    invariant(c);

    auto nRecords = records->size();
    invariant(nRecords != 0);

    int64_t totalLength = 0;
    for (size_t i = 0; i < nRecords; i++) {
        auto& record = (*records)[i];
        totalLength += record.data.size();
        OpStats opStats{};
        Status status = wtInsertRecord(opCtx, wtRu, c, record, opStats);
        if (!status.isOK()) {
            return status;
        }
    }
    _changeNumRecordsAndDataSize(nRecords, totalLength);
    return Status::OK();
}

Status SpillRecordStore::_updateRecord(OperationContext* opCtx,
                                       const RecordId& id,
                                       const char* data,
                                       int len) {
    auto& wtRu = SpillRecoveryUnit::get(getRecoveryUnit(nullptr));
    OpStats opStats{};
    auto status = wtUpdateRecord(opCtx, wtRu, id, data, len, opStats);
    if (!status.isOK()) {
        return status;
    }

    auto sizeDiff = opStats.newValueLength - opStats.oldValueLength;
    _changeNumRecordsAndDataSize(0, sizeDiff);
    return Status::OK();
}

Status SpillRecordStore::_truncate(OperationContext* opCtx) {
    auto& wtRu = SpillRecoveryUnit::get(getRecoveryUnit(nullptr));
    auto status = wtTruncate(opCtx, wtRu);
    if (!status.isOK()) {
        return status;
    }

    _changeNumRecordsAndDataSize(-numRecords(), -dataSize());
    return Status::OK();
}

Status SpillRecordStore::_rangeTruncate(OperationContext* opCtx,
                                        const RecordId& minRecordId,
                                        const RecordId& maxRecordId,
                                        int64_t hintDataSizeDiff,
                                        int64_t hintNumRecordsDiff) {
    auto& wtRu = SpillRecoveryUnit::get(getRecoveryUnit(nullptr));
    auto status = wtRangeTruncate(opCtx, wtRu, minRecordId, maxRecordId);
    if (!status.isOK()) {
        return status;
    }

    _changeNumRecordsAndDataSize(hintNumRecordsDiff, hintDataSizeDiff);
    return Status::OK();
}

void SpillRecordStore::_changeNumRecordsAndDataSize(int64_t numRecordDiff, int64_t dataSizeDiff) {
    _sizeInfo.numRecords.addAndFetch(numRecordDiff);
    _sizeInfo.dataSize.addAndFetch(dataSizeDiff);
}

SpillRecordStoreCursor::SpillRecordStoreCursor(OperationContext* opCtx,
                                               const SpillRecordStore& rs,
                                               bool forward,
                                               SpillRecoveryUnit* wtRu)
    : WiredTigerRecordStoreCursorBase(opCtx, rs, forward), _wtRu(wtRu) {
    init();
}

RecoveryUnit& SpillRecordStoreCursor::getRecoveryUnit() const {
    return *_wtRu;
}

}  // namespace mongo
