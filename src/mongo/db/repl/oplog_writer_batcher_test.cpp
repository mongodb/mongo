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

#include "mongo/db/repl/oplog_batch.h"
#include "mongo/db/repl/oplog_batcher_test_fixture.h"
#include "mongo/db/repl/oplog_writer_batcher.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"

namespace mongo {
namespace repl {

class OplogWriterBufferMock : public OplogBuffer {
    OplogWriterBufferMock(const OplogBufferMock&) = delete;
    OplogWriterBufferMock& operator=(const OplogBufferMock&) = delete;

public:
    OplogWriterBufferMock() = default;
    virtual ~OplogWriterBufferMock() = default;
    void startup(OperationContext*) {
        MONGO_UNIMPLEMENTED;
    }

    void shutdown(OperationContext* opCtx) {
        MONGO_UNIMPLEMENTED;
    }

    void push(OperationContext*, Batch::const_iterator begin, Batch::const_iterator end) {
        MONGO_UNIMPLEMENTED;
    }

    void push_forTest(OplogBatchBSONObj& batch) {
        _queue.push(batch);
        _notEmptyCv.notify_one();
    }

    void waitForSpace(OperationContext*, std::size_t size) {
        MONGO_UNIMPLEMENTED;
    }

    bool isEmpty() const {
        return _queue.empty();
    }

    std::size_t getMaxSize() const {
        MONGO_UNIMPLEMENTED;
    }

    std::size_t getSize() const {
        MONGO_UNIMPLEMENTED;
    }

    std::size_t getCount() const {
        MONGO_UNIMPLEMENTED;
    }

    void clear(OperationContext*) {
        MONGO_UNIMPLEMENTED;
    }

    bool tryPop(OperationContext*, Value* value) {
        MONGO_UNIMPLEMENTED;
    }

    bool tryPopBatch(OperationContext* opCtx, OplogBatchBSONObj* batch) {
        if (_queue.empty()) {
            return false;
        }
        *batch = std::move(_queue.front());
        _queue.pop();
        return true;
    }

    bool waitForDataFor(Milliseconds waitDuration, Interruptible* interruptible) {
        stdx::unique_lock<Latch> lk(_notEmptyMutex);
        interruptible->waitForConditionOrInterruptFor(
            _notEmptyCv, lk, waitDuration, [&] { return !_queue.empty(); });
        return !_queue.empty();
    }

    bool waitForDataUntil(Date_t deadline, Interruptible* interruptible) {
        MONGO_UNIMPLEMENTED;
    }

    bool peek(OperationContext*, Value* value) {
        MONGO_UNIMPLEMENTED;
    }

    boost::optional<OplogBuffer::Value> lastObjectPushed(OperationContext*) const {
        MONGO_UNIMPLEMENTED;
    }

    Mutex _notEmptyMutex = MONGO_MAKE_LATCH("OplogWriterBuffer::mutex");
    stdx::condition_variable _notEmptyCv;
    std::queue<OplogBatchBSONObj> _queue;
};

BSONObj makeNoopOplogEntry(OpTime opTime) {
    auto oplogEntry = repl::DurableOplogEntry(
        opTime,                                                    // optime
        OpTypeEnum ::kNoop,                                        // opType
        NamespaceString::createNamespaceString_forTest("test.t"),  // namespace
        boost::none,                                               // uuid
        boost::none,                                               // fromMigrate
        boost::none,                                               // checkExistenceForDiffInsert
        repl::OplogEntry::kOplogVersion,                           // version
        BSONObj(),                                                 // o
        boost::none,                                               // o2
        {},                                                        // sessionInfo
        boost::none,                                               // upsert
        Date_t(),                                                  // wall clock time
        {},                                                        // statement ids
        boost::none,   // optime of previous write within same transaction
        boost::none,   // pre-image optime
        boost::none,   // post-image optime
        boost::none,   // ShardId of resharding recipient
        boost::none,   // _id
        boost::none);  // needsRetryImage
    return oplogEntry.toBSON();
}

BSONObj makeNoopOplogEntry(Seconds seconds) {
    return makeNoopOplogEntry({{seconds, 0}, 1LL});
}

class OplogWriterBatcherTest : public ServiceContextMongoDTest {
public:
    explicit OplogWriterBatcherTest(Options options = {})
        : ServiceContextMongoDTest(options.useReplSettings(true)) {}

    void setUp() override;
    void tearDown() override;

    OperationContext* opCtx() const;

protected:
    ServiceContext* _serviceContext;
    ServiceContext::UniqueOperationContext _opCtxHolder;
};

void OplogWriterBatcherTest::setUp() {
    ServiceContextMongoDTest::setUp();

    _opCtxHolder = makeOperationContext();
}

void OplogWriterBatcherTest::tearDown() {
    _opCtxHolder = {};
    ServiceContextMongoDTest::tearDown();
}

OperationContext* OplogWriterBatcherTest::opCtx() const {
    return _opCtxHolder.get();
}

TEST_F(OplogWriterBatcherTest, EmptyBuffer) {
    OplogWriterBufferMock writerBuffer;
    OplogWriterBatcher writerBatcher(&writerBuffer);
    ASSERT_TRUE(writerBatcher.getNextBatch(opCtx(), Seconds(1)).empty());
}

TEST_F(OplogWriterBatcherTest, MergeBatches) {
    OplogWriterBufferMock writerBuffer;
    OplogWriterBatcher writerBatcher(&writerBuffer);
    OplogBatchBSONObj batch1({makeNoopOplogEntry(Seconds(123)), makeNoopOplogEntry(Seconds(456))},
                             2);
    OplogBatchBSONObj batch2({makeNoopOplogEntry(Seconds(789))}, 1);
    writerBuffer.push_forTest(batch1);
    writerBuffer.push_forTest(batch2);
    auto batch = writerBatcher.getNextBatch(opCtx(), Seconds(1)).releaseBatch();
    ASSERT_EQ(3, batch.size());
}

TEST_F(OplogWriterBatcherTest, WriterBatchSizeInBytes) {
    OplogWriterBufferMock writerBuffer;
    OplogWriterBatcher writerBatcher(&writerBuffer);
    OplogBatchBSONObj batch1({makeNoopOplogEntry(Seconds(123))}, 16 * 1024 * 1024 /*16MB*/);
    OplogBatchBSONObj batch2({makeNoopOplogEntry(Seconds(123))}, 1);
    OplogBatchBSONObj batch3({makeNoopOplogEntry(Seconds(123))}, 1);
    OplogBatchBSONObj batch4({makeNoopOplogEntry(Seconds(123))}, 16 * 1024 * 1024 /*16MB*/);
    writerBuffer.push_forTest(batch1);
    writerBuffer.push_forTest(batch2);
    writerBuffer.push_forTest(batch3);
    writerBuffer.push_forTest(batch4);
    // Once we can form a batch whose bytes size in (16MB, 32MB], we return immediately.
    ASSERT_EQ(16 * 1024 * 1024 + 1, writerBatcher.getNextBatch(opCtx(), Seconds(1)).getByteSize());
    ASSERT_EQ(16 * 1024 * 1024 + 1, writerBatcher.getNextBatch(opCtx(), Seconds(1)).getByteSize());
    ASSERT_TRUE(writerBatcher.getNextBatch(opCtx(), Seconds(1)).empty());
}

DEATH_TEST_F(OplogWriterBatcherTest, BatchFromBufferSizeLimit, "batchSize <= kMinWriterBatchSize") {
    OplogWriterBufferMock writerBuffer;
    OplogWriterBatcher writerBatcher(&writerBuffer);
    OplogBatchBSONObj batch1({makeNoopOplogEntry(Seconds(123))}, 16 * 1024 * 1024 /*16MB*/ + 1);
    writerBuffer.push_forTest(batch1);
    writerBatcher.getNextBatch(opCtx(), Seconds(1));
}

TEST_F(OplogWriterBatcherTest, BatcherWillReturnNewBatchInBufferWhenNoDataAvailable) {
    OplogWriterBufferMock writerBuffer;
    OplogWriterBatcher writerBatcher(&writerBuffer);
    OplogBatchBSONObj batch1({makeNoopOplogEntry(Seconds(123))}, 16 * 1024 * 1024 /*16MB*/);

    stdx::thread pushBufferThread([&]() {
        Client::initThread("pushBufferThread", getGlobalServiceContext()->getService());
        // Wait 1s so the batcher can pop batch1 and wait more data.
        sleepmillis(1000);
        writerBuffer.push_forTest(batch1);
    });

    // Make the maxWaitTime longer to give another thread enough time to push data into buffer.
    auto batch = writerBatcher.getNextBatch(opCtx(), Seconds(10));

    pushBufferThread.join();

    ASSERT_EQ(16 * 1024 * 1024, batch.getByteSize());
}

TEST_F(OplogWriterBatcherTest, BatcherWillNotWaitForMoreDataWhenAlreadyHaveBatch) {
    OplogWriterBufferMock writerBuffer;
    OplogWriterBatcher writerBatcher(&writerBuffer);
    OplogBatchBSONObj batch1({makeNoopOplogEntry(Seconds(123))}, 16 * 1024 * 1024 /*16MB*/);
    OplogBatchBSONObj batch2({makeNoopOplogEntry(Seconds(123))}, 16 * 1024 * 1024 /*16MB*/);
    writerBuffer.push_forTest(batch1);

    stdx::thread pushBufferThread([&]() {
        Client::initThread("pushBufferThread", getGlobalServiceContext()->getService());
        // Wait 5s so the other thread can read the first batch and return.
        sleepmillis(5000);
        writerBuffer.push_forTest(batch2);
    });

    // Make the maxWaitTime longer to give another thread enough time to push data into buffer.
    auto batch = writerBatcher.getNextBatch(opCtx(), Seconds(10));

    pushBufferThread.join();

    // The batcher should not wait for the batch pushed by pushBufferThread.
    ASSERT_EQ(16 * 1024 * 1024, batch.getByteSize());
}

}  // namespace repl
}  // namespace mongo
