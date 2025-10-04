/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
