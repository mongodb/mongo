// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/modules.h"

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
