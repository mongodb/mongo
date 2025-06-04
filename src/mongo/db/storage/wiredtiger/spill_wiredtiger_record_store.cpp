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

#include "mongo/db/storage/wiredtiger/spill_wiredtiger_record_store.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/spill_wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

SpillWiredTigerRecordStore::SpillWiredTigerRecordStore(SpillWiredTigerKVEngine* kvEngine,
                                                       Params params)
    : WiredTigerRecordStoreBase(std::move(params.baseParams)),
      _kvEngine(kvEngine),
      _wtRu(std::make_unique<SpillRecoveryUnit>(&kvEngine->getConnection())),
      _sizeInfo(0, 0) {}

SpillWiredTigerRecordStore::~SpillWiredTigerRecordStore() {
    // TODO(SERVER-103273): Truncate and drop the table here.
    LOGV2_DEBUG(10158010,
                1,
                "~SpillWiredTigerRecordStore for temporary ident: {getIdent}",
                "getIdent"_attr = getIdent());
}

std::unique_ptr<SeekableRecordCursor> SpillWiredTigerRecordStore::getCursor(OperationContext* opCtx,
                                                                            RecoveryUnit& ru,
                                                                            bool forward) const {
    return std::make_unique<SpillWiredTigerRecordStoreCursor>(opCtx, *_wtRu, *this, forward);
}

long long SpillWiredTigerRecordStore::dataSize() const {
    auto dataSize = _sizeInfo.dataSize.load();
    return dataSize > 0 ? dataSize : 0;
}

long long SpillWiredTigerRecordStore::numRecords() const {
    auto numRecords = _sizeInfo.numRecords.load();
    return numRecords > 0 ? numRecords : 0;
}

int64_t SpillWiredTigerRecordStore::storageSize(RecoveryUnit& ru,
                                                BSONObjBuilder* extraInfo,
                                                int infoLevel) const {
    auto& wtRu = SpillRecoveryUnit::get(getRecoveryUnit(ru));
    WiredTigerSession* session = wtRu.getSessionNoTxn();
    auto result = WiredTigerUtil::getStatisticsValue(
        *session, "statistics:" + getURI(), "statistics=(size)", WT_STAT_DSRC_BLOCK_SIZE);
    uassertStatusOK(result.getStatus());
    return result.getValue();
}

RecoveryUnit& SpillWiredTigerRecordStore::getRecoveryUnit(RecoveryUnit& ru) const {
    return *_wtRu;
}

void SpillWiredTigerRecordStore::_deleteRecord(OperationContext* opCtx,
                                               RecoveryUnit& ru,
                                               const RecordId& id) {
    auto& wtRu = SpillRecoveryUnit::get(getRecoveryUnit(ru));
    OpStats opStats{};
    wtDeleteRecord(opCtx, wtRu, id, opStats);
    _changeNumRecordsAndDataSize(-1, -opStats.oldValueLength);
}

Status SpillWiredTigerRecordStore::_insertRecords(OperationContext* opCtx,
                                                  RecoveryUnit& ru,
                                                  std::vector<Record>* records,
                                                  const std::vector<Timestamp>&) {
    auto& wtRu = SpillRecoveryUnit::get(getRecoveryUnit(ru));
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

Status SpillWiredTigerRecordStore::_updateRecord(
    OperationContext* opCtx, RecoveryUnit& ru, const RecordId& id, const char* data, int len) {
    auto& wtRu = SpillRecoveryUnit::get(getRecoveryUnit(ru));
    OpStats opStats{};
    auto status = wtUpdateRecord(opCtx, wtRu, id, data, len, opStats);
    if (!status.isOK()) {
        return status;
    }

    auto sizeDiff = opStats.newValueLength - opStats.oldValueLength;
    _changeNumRecordsAndDataSize(0, sizeDiff);
    return Status::OK();
}

Status SpillWiredTigerRecordStore::_truncate(OperationContext* opCtx, RecoveryUnit& ru) {
    auto& wtRu = SpillRecoveryUnit::get(getRecoveryUnit(ru));
    auto status = wtTruncate(opCtx, wtRu);
    if (!status.isOK()) {
        return status;
    }

    _changeNumRecordsAndDataSize(-numRecords(), -dataSize());
    return Status::OK();
}

Status SpillWiredTigerRecordStore::_rangeTruncate(OperationContext* opCtx,
                                                  RecoveryUnit& ru,
                                                  const RecordId& minRecordId,
                                                  const RecordId& maxRecordId,
                                                  int64_t hintDataSizeDiff,
                                                  int64_t hintNumRecordsDiff) {
    auto& wtRu = SpillRecoveryUnit::get(getRecoveryUnit(ru));
    auto status = wtRangeTruncate(opCtx, wtRu, minRecordId, maxRecordId);
    if (!status.isOK()) {
        return status;
    }

    _changeNumRecordsAndDataSize(hintNumRecordsDiff, hintDataSizeDiff);
    return Status::OK();
}

void SpillWiredTigerRecordStore::_changeNumRecordsAndDataSize(int64_t numRecordDiff,
                                                              int64_t dataSizeDiff) {
    _sizeInfo.numRecords.addAndFetch(numRecordDiff);
    _sizeInfo.dataSize.addAndFetch(dataSizeDiff);
}

SpillWiredTigerRecordStoreCursor::SpillWiredTigerRecordStoreCursor(
    OperationContext* opCtx,
    SpillRecoveryUnit& ru,
    const SpillWiredTigerRecordStore& rs,
    bool forward)
    : WiredTigerRecordStoreCursorBase(opCtx, ru, rs, forward), _wtRu(ru) {
    init();
}

RecoveryUnit& SpillWiredTigerRecordStoreCursor::getRecoveryUnit() const {
    return _wtRu;
}

}  // namespace mongo
