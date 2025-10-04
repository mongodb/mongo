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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/storage/record_store.h"

#include <vector>

namespace mongo {

/**
 * A class that writes records to a temporary record store. Performs writes in batches and
 * accumulates statistics.
 */
class SpillTableBatchWriter {
public:
    SpillTableBatchWriter(ExpressionContext* expCtx, SpillTable& spillTable)
        : _expCtx(expCtx), _spillTable(spillTable) {}

    void write(RecordId recordId, BSONObj obj);

    /**
     * The caller is the owner of the data in recordData and should make sure to keep it alive until
     * the data has been flushed.
     */
    void write(RecordId recordId, RecordData recordData);

    void flush();

    int64_t writtenRecords() const {
        return _writtenRecords;
    }

    int64_t writtenBytes() const {
        return _writtenBytes;
    }

private:
    static constexpr size_t kMaxWriteRecordCount = 1000;
    static constexpr size_t kMaxWriteRecordSize = 16 * 1024 * 1024;

    ExpressionContext* _expCtx;
    SpillTable& _spillTable;

    std::vector<Record> _records;
    std::vector<BSONObj> _ownedObjects;
    size_t _batchSize = 0;

    int64_t _writtenRecords = 0;
    int64_t _writtenBytes = 0;
};

}  // namespace mongo
