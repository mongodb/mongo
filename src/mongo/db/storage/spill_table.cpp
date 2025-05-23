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

#include "mongo/db/storage/spill_table.h"

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

long long SpillTable::dataSize() const {
    return _rs->dataSize();
}

long long SpillTable::numRecords() const {
    return _rs->numRecords();
}

int64_t SpillTable::storageSize(RecoveryUnit& ru) const {
    return _rs->storageSize(ru);
}

Status SpillTable::insertRecords(OperationContext* opCtx, std::vector<Record>* records) {
    std::vector<Timestamp> timestamps(records->size());
    return _rs->insertRecords(opCtx, records, timestamps);
}

bool SpillTable::findRecord(OperationContext* opCtx, const RecordId& rid, RecordData* out) const {
    return _rs->findRecord(opCtx, rid, out);
}

Status SpillTable::updateRecord(OperationContext* opCtx,
                                const RecordId& rid,
                                const char* data,
                                int len) {
    return _rs->updateRecord(opCtx, rid, data, len);
}

void SpillTable::deleteRecord(OperationContext* opCtx, const RecordId& rid) {
    _rs->deleteRecord(opCtx, rid);
}

std::unique_ptr<SeekableRecordCursor> SpillTable::getCursor(OperationContext* opCtx,
                                                            bool forward) const {
    return _rs->getCursor(opCtx, forward);
}

Status SpillTable::truncate(OperationContext* opCtx) {
    return _rs->truncate(opCtx);
}

Status SpillTable::rangeTruncate(OperationContext* opCtx,
                                 const RecordId& minRecordId,
                                 const RecordId& maxRecordId,
                                 int64_t hintDataSizeIncrement,
                                 int64_t hintNumRecordsIncrement) {
    return _rs->rangeTruncate(
        opCtx, minRecordId, maxRecordId, hintDataSizeIncrement, hintNumRecordsIncrement);
}

}  // namespace mongo
