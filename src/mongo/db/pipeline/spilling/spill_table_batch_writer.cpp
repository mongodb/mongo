// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/spilling/spill_table_batch_writer.h"

namespace mongo {

void SpillTableBatchWriter::write(RecordId recordId, BSONObj obj) {
    _ownedObjects.push_back(obj.getOwned());
    const BSONObj& ownedObj = _ownedObjects.back();
    write(recordId, RecordData{ownedObj.objdata(), ownedObj.objsize()});
}

void SpillTableBatchWriter::write(RecordId recordId, RecordData recordData) {
    _records.emplace_back(Record{std::move(recordId), recordData});
    _batchSize += _records.back().id.memUsage() + _records.back().data.size();
    if (_records.size() > kMaxWriteRecordCount || _batchSize > kMaxWriteRecordSize) {
        flush();
    }
}

void SpillTableBatchWriter::flush() {
    if (_records.empty()) {
        return;
    }

    _expCtx->getMongoProcessInterface()->writeRecordsToSpillTable(_expCtx, _spillTable, &_records);

    _writtenRecords += _records.size();
    _writtenBytes += _batchSize;

    _records.clear();
    _ownedObjects.clear();
    _batchSize = 0;
}

}  // namespace mongo
