/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/op_observer/batched_write_policy.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/unittest/unittest.h"

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class BatchedWritePolicyTest : public unittest::Test {
protected:
    void setUp() override {}
    void tearDown() override {}
    explicit BatchedWritePolicyTest() {}
};

static const size_t maxSizeBytes = BSONObjMaxUserSize - 1000;
static const size_t maxNumDocs = 500;

void _generateRecords(size_t numRecords, std::deque<Record>& records) {
    for (size_t i = 0; i < numRecords; i++) {
        auto rid = RecordId(i);
        auto obj = BSON("a" << std::to_string(i));
        auto rd = RecordData(obj.objdata(), obj.objsize());
        rd.makeOwned();
        auto r = Record{rid, rd};
        records.push_back(std::move(r));
    }
}


class CursorMock : public RecordCursor {
public:
    CursorMock(std::deque<Record>* records) : _records(records) {}

    ~CursorMock() override {}

    boost::optional<Record> next() override {
        if (_records->empty()) {
            return boost::none;
        }
        Record next = _records->front();
        _records->pop_front();
        return next;
    }

    void save() override {}
    bool restore(RecoveryUnit& ru, bool tolerateCappedRepositioning) override {
        return true;
    }
    void detachFromOperationContext() override {}
    void reattachToOperationContext(OperationContext* opCtx) override {}
    void setSaveStorageCursorOnDetachFromOperationContext(bool) override {}

private:
    std::deque<Record>* _records;
};

TEST_F(BatchedWritePolicyTest, TooFewDocumentsTest) {
    // Neither length or size limit reached.
    std::vector<InsertStatement> stmts;
    std::deque<Record> records;
    _generateRecords(3, records);
    auto rp = &records;
    auto cm = CursorMock(rp);
    auto record = cm.next();

    buildBatchedWritesWithPolicy(maxSizeBytes, 4, [&cm]() { return cm.next(); }, record, stmts);
    ASSERT_EQ(3, stmts.size());

    // Exhausted Cursor.
    stmts.clear();
    buildBatchedWritesWithPolicy(maxSizeBytes, 4, [&cm]() { return cm.next(); }, record, stmts);
    ASSERT_EQ(0, stmts.size());
}

TEST_F(BatchedWritePolicyTest, TooManyDocumentsTest) {
    // Break up a batch if the max number of documents is exceeded.
    std::vector<InsertStatement> stmts;
    std::deque<Record> records;
    _generateRecords(5, records);
    auto rp = &records;
    auto cm = CursorMock(rp);
    auto record = cm.next();

    buildBatchedWritesWithPolicy(maxSizeBytes, 4, [&cm]() { return cm.next(); }, record, stmts);
    ASSERT_EQ(4, stmts.size());

    stmts.clear();
    buildBatchedWritesWithPolicy(maxSizeBytes, 4, [&cm]() { return cm.next(); }, record, stmts);
    ASSERT_EQ(1, stmts.size());
}

TEST_F(BatchedWritePolicyTest, TooManyDocumentsMultiTest) {
    // Break up a batch if the max number of documents is exceeded.
    size_t batchSize = 3;
    size_t numRecords = 20;
    size_t numFullBatches = numRecords / batchSize;
    std::vector<InsertStatement> stmts;
    std::deque<Record> records;
    _generateRecords(numRecords, records);
    auto rp = &records;
    auto cm = CursorMock(rp);
    auto record = cm.next();

    for (size_t i = 0; i < numFullBatches; i++) {
        buildBatchedWritesWithPolicy(
            maxSizeBytes, batchSize, [&cm]() { return cm.next(); }, record, stmts);
        ASSERT_EQ(batchSize, stmts.size());
        stmts.clear();
    }
    buildBatchedWritesWithPolicy(
        maxSizeBytes, batchSize, [&cm]() { return cm.next(); }, record, stmts);
    ASSERT_EQ(static_cast<size_t>(numRecords % batchSize), stmts.size());
}

TEST_F(BatchedWritePolicyTest, TooManyBigDocumentsTest) {
    // Break up a batch if the max size is exceeded.
    std::vector<InsertStatement> stmts;
    std::deque<Record> records;
    _generateRecords(5, records);
    auto rp = &records;
    auto cm = CursorMock(rp);
    auto record = cm.next();

    buildBatchedWritesWithPolicy(4 * 14, maxNumDocs, [&cm]() { return cm.next(); }, record, stmts);
    ASSERT_EQ(4, stmts.size());

    stmts.clear();
    buildBatchedWritesWithPolicy(4 * 14, maxNumDocs, [&cm]() { return cm.next(); }, record, stmts);
    ASSERT_EQ(1, stmts.size());
}

TEST_F(BatchedWritePolicyTest, TooBigDocumentTest) {
    // Each document exceeds the byte limit for a batch, but still is valid
    // (below 16MB limit). Therefore, each batch will contain one document.
    std::vector<InsertStatement> stmts;
    std::deque<Record> records;
    _generateRecords(3, records);
    auto rp = &records;
    auto cm = CursorMock(rp);
    auto record = cm.next();

    buildBatchedWritesWithPolicy(1, 10, [&cm]() { return cm.next(); }, record, stmts);
    ASSERT_EQ(1, stmts.size());

    stmts.clear();
    buildBatchedWritesWithPolicy(1, 10, [&cm]() { return cm.next(); }, record, stmts);
    ASSERT_EQ(1, stmts.size());

    stmts.clear();
    buildBatchedWritesWithPolicy(1, 10, [&cm]() { return cm.next(); }, record, stmts);
    ASSERT_EQ(1, stmts.size());
}

TEST_F(BatchedWritePolicyTest, UnBatchedCappedCollectionTest) {
    // Simulating a Capped Collection where inserts can't be batched.
    std::vector<InsertStatement> stmts;
    std::deque<Record> records;
    _generateRecords(4, records);
    auto rp = &records;
    auto cm = CursorMock(rp);
    auto record = cm.next();

    buildBatchedWritesWithPolicy(
        maxSizeBytes, 2, [&cm]() { return cm.next(); }, record, stmts, /*canBeBatched=*/false);
    ASSERT_EQ(1, stmts.size());

    stmts.clear();
    buildBatchedWritesWithPolicy(
        maxSizeBytes, 2, [&cm]() { return cm.next(); }, record, stmts, /*canBeBatched=*/false);
    ASSERT_EQ(1, stmts.size());

    stmts.clear();
    buildBatchedWritesWithPolicy(
        maxSizeBytes, 2, [&cm]() { return cm.next(); }, record, stmts, /*canBeBatched=*/false);
    ASSERT_EQ(1, stmts.size());

    stmts.clear();
    buildBatchedWritesWithPolicy(
        maxSizeBytes, 2, [&cm]() { return cm.next(); }, record, stmts, /*canBeBatched=*/false);
    ASSERT_EQ(1, stmts.size());
}

}  // namespace
}  // namespace mongo
