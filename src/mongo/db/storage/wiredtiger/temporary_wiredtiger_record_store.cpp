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

#include "mongo/db/storage/wiredtiger/temporary_wiredtiger_record_store.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/temporary_wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

TemporaryWiredTigerRecordStore::TemporaryWiredTigerRecordStore(
    TemporaryWiredTigerKVEngine* kvEngine,
    ServiceContext::UniqueOperationContext opCtx,
    Params params)
    : WiredTigerRecordStoreBase(std::move(params.baseParams)),
      _kvEngine(kvEngine),
      _opCtx(std::move(opCtx)),
      _sizeInfo(0, 0) {
    shard_role_details::setRecoveryUnit(_opCtx.get(),
                                        std::unique_ptr<RecoveryUnit>(kvEngine->newRecoveryUnit()),
                                        WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
}


TemporaryWiredTigerRecordStore::~TemporaryWiredTigerRecordStore() {
    // TODO(SERVER-103273): Truncate and drop the table here.
    LOGV2_DEBUG(10158010,
                1,
                "~TemporaryWiredTigerRecordStore for temporary ident: {getIdent}",
                "getIdent"_attr = getIdent());
}

std::unique_ptr<SeekableRecordCursor> TemporaryWiredTigerRecordStore::getCursor(
    OperationContext*, bool forward) const {
    return std::make_unique<WiredTigerRecordStoreCursor>(_opCtx.get(), *this, forward);
}

long long TemporaryWiredTigerRecordStore::dataSize() const {
    auto dataSize = _sizeInfo.dataSize.load();
    return dataSize > 0 ? dataSize : 0;
}

long long TemporaryWiredTigerRecordStore::numRecords() const {
    auto numRecords = _sizeInfo.numRecords.load();
    return numRecords > 0 ? numRecords : 0;
}

int64_t TemporaryWiredTigerRecordStore::storageSize(RecoveryUnit& ru,
                                                    BSONObjBuilder* extraInfo,
                                                    int infoLevel) const {
    auto& wtRu = WiredTigerRecoveryUnit::get(*shard_role_details::getRecoveryUnit(_opCtx.get()));
    WiredTigerSession* session = wtRu.getSessionNoTxn();
    auto result = WiredTigerUtil::getStatisticsValue(
        *session, "statistics:" + getURI(), "statistics=(size)", WT_STAT_DSRC_BLOCK_SIZE);
    uassertStatusOK(result.getStatus());
    return result.getValue();
}

void TemporaryWiredTigerRecordStore::_deleteRecord(OperationContext*, const RecordId& id) {
    auto& wtRu = WiredTigerRecoveryUnit::get(*shard_role_details::getRecoveryUnit(_opCtx.get()));
    OpStats opStats{};
    wtDeleteRecord(_opCtx.get(), wtRu, id, opStats);

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx.get());
    metricsCollector.incrementOneDocWritten(_uri, opStats.oldValueLength + opStats.keyLength);

    _changeNumRecordsAndDataSize(-1, -opStats.oldValueLength);
}

Status TemporaryWiredTigerRecordStore::_insertRecords(OperationContext*,
                                                      std::vector<Record>* records,
                                                      const std::vector<Timestamp>&) {
    auto& wtRu = WiredTigerRecoveryUnit::get(*shard_role_details::getRecoveryUnit(_opCtx.get()));
    auto cursorParams = getWiredTigerCursorParams(wtRu, _tableId, _overwrite);
    WiredTigerCursor curwrap(std::move(cursorParams), _uri, *wtRu.getSession());
    wtRu.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);

    auto nRecords = records->size();
    invariant(nRecords != 0);

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx.get());
    int64_t totalLength = 0;
    for (size_t i = 0; i < nRecords; i++) {
        auto& record = (*records)[i];
        totalLength += record.data.size();
        OpStats opStats{};
        Status status = wtInsertRecord(_opCtx.get(), wtRu, c, record, opStats);
        if (!status.isOK()) {
            return status;
        }

        // Increment metrics for each insert separately, as opposed to outside of the loop. The API
        // requires that each record be accounted for separately.
        metricsCollector.incrementOneDocWritten(_uri, opStats.newValueLength + opStats.keyLength);
    }
    _changeNumRecordsAndDataSize(nRecords, totalLength);
    return Status::OK();
}

Status TemporaryWiredTigerRecordStore::_updateRecord(OperationContext*,
                                                     const RecordId& id,
                                                     const char* data,
                                                     int len) {
    auto& wtRu = WiredTigerRecoveryUnit::get(*shard_role_details::getRecoveryUnit(_opCtx.get()));
    OpStats opStats{};
    auto status = wtUpdateRecord(_opCtx.get(), wtRu, id, data, len, opStats);
    if (!status.isOK()) {
        return status;
    }

    // For updates that don't modify the document size, they should count as at least one unit, so
    // just attribute them as 1-byte modifications for simplicity.
    auto sizeDiff = opStats.newValueLength - opStats.oldValueLength;
    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx.get());
    metricsCollector.incrementOneDocWritten(_uri, std::max((int64_t)1, std::abs(sizeDiff)));

    _changeNumRecordsAndDataSize(0, sizeDiff);
    return Status::OK();
}

Status TemporaryWiredTigerRecordStore::_truncate(OperationContext*) {
    auto& wtRu = WiredTigerRecoveryUnit::get(*shard_role_details::getRecoveryUnit(_opCtx.get()));
    auto status = wtTruncate(_opCtx.get(), wtRu);
    if (!status.isOK()) {
        return status;
    }

    _changeNumRecordsAndDataSize(-numRecords(), -dataSize());
    return Status::OK();
}

Status TemporaryWiredTigerRecordStore::_rangeTruncate(OperationContext*,
                                                      const RecordId& minRecordId,
                                                      const RecordId& maxRecordId,
                                                      int64_t hintDataSizeDiff,
                                                      int64_t hintNumRecordsDiff) {
    auto& wtRu = WiredTigerRecoveryUnit::get(*shard_role_details::getRecoveryUnit(_opCtx.get()));
    auto status = wtRangeTruncate(_opCtx.get(), wtRu, minRecordId, maxRecordId);
    if (!status.isOK()) {
        return status;
    }

    _changeNumRecordsAndDataSize(hintNumRecordsDiff, hintDataSizeDiff);
    return Status::OK();
}

void TemporaryWiredTigerRecordStore::_changeNumRecordsAndDataSize(int64_t numRecordDiff,
                                                                  int64_t dataSizeDiff) {
    _sizeInfo.numRecords.addAndFetch(numRecordDiff);
    _sizeInfo.dataSize.addAndFetch(dataSizeDiff);
}

}  // namespace mongo
