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

#include "mongo/db/repl/oplog_buffer_batched_queue.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"

namespace mongo {
namespace repl {

BSONObj makeNoopOplogEntry(int t) {
    auto oplogEntry = repl::DurableOplogEntry(
        OpTime(Timestamp(t, 1), 1),                                // optime
        OpTypeEnum::kNoop,                                         // opType
        NamespaceString::createNamespaceString_forTest("test.t"),  // namespace
        boost::none,                                               // uuid
        boost::none,                                               // fromMigrate
        boost::none,                                               // checkExistenceForDiffInsert
        repl::OplogEntry::kOplogVersion,                           // version
        BSON("count" << t),                                        // o
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

class OplogBufferBatchedQueueTest : public ServiceContextMongoDTest {
public:
    explicit OplogBufferBatchedQueueTest(Options options = {})
        : ServiceContextMongoDTest(options.useReplSettings(true)) {}

    void setUp() override;
    void tearDown() override;

    OperationContext* opCtx() const;

protected:
    ServiceContext* _serviceContext;
    ServiceContext::UniqueOperationContext _opCtxHolder;
};

void OplogBufferBatchedQueueTest::setUp() {
    ServiceContextMongoDTest::setUp();
    _serviceContext = getServiceContext();
    _opCtxHolder = makeOperationContext();
}

void OplogBufferBatchedQueueTest::tearDown() {
    _opCtxHolder = {};
    ServiceContextMongoDTest::tearDown();
}

OperationContext* OplogBufferBatchedQueueTest::opCtx() const {
    return _opCtxHolder.get();
}

TEST_F(OplogBufferBatchedQueueTest, PushEmptyBatch) {
    OplogBufferBatchedQueue buffer(1024);
    buffer.startup(opCtx());

    std::vector<BSONObj> ops;
    buffer.push(opCtx(), ops.begin(), ops.end(), 0);

    ASSERT(buffer.isEmpty());
    ASSERT_EQ(0, buffer.getSize());
    ASSERT_EQ(0, buffer.getCount());

    OplogBatch<BSONObj> firstBatch;
    ASSERT_FALSE(buffer.tryPopBatch(opCtx(), &firstBatch));
}

TEST_F(OplogBufferBatchedQueueTest, BasicBatchedQueueOperations) {
    OplogBufferBatchedQueue buffer(1024);
    buffer.startup(opCtx());

    // Push two batches.
    std::vector<BSONObj> ops1;
    ops1.push_back(makeNoopOplogEntry(1));
    ops1.push_back(makeNoopOplogEntry(2));

    size_t size1 = 16;
    buffer.push(opCtx(), ops1.begin(), ops1.end(), size1);

    std::vector<BSONObj> ops2;
    ops2.push_back(makeNoopOplogEntry(3));

    size_t size2 = 32;
    buffer.push(opCtx(), ops2.begin(), ops2.end(), size2);

    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(size1 + size2, buffer.getSize());
    ASSERT_EQ(ops1.size() + ops2.size(), buffer.getCount());

    // Pop both batches.
    OplogBatch<BSONObj> firstBatch;
    ASSERT(buffer.tryPopBatch(opCtx(), &firstBatch));
    ASSERT_EQ(size1, firstBatch.byteSize());
    ASSERT_EQ(ops1.size(), firstBatch.count());
    ASSERT_BSONOBJ_EQ(ops1[0], firstBatch.getBatch()[0]);
    ASSERT_BSONOBJ_EQ(ops1[1], firstBatch.getBatch()[1]);

    OplogBatch<BSONObj> secondBatch;
    ASSERT(buffer.tryPopBatch(opCtx(), &secondBatch));
    ASSERT_EQ(size2, secondBatch.byteSize());
    ASSERT_EQ(ops2.size(), secondBatch.count());
    ASSERT_BSONOBJ_EQ(ops2[0], secondBatch.getBatch()[0]);

    // Popped both batches, now the buffer should be empty.
    ASSERT(buffer.isEmpty());
    ASSERT_EQ(0, buffer.getSize());
    ASSERT_EQ(0, buffer.getCount());

    OplogBatch<BSONObj> thirdBatch;
    ASSERT_FALSE(buffer.tryPopBatch(opCtx(), &thirdBatch));
}

TEST_F(OplogBufferBatchedQueueTest, ClearAndShutdownBuffer) {
    OplogBufferBatchedQueue buffer(1024);
    buffer.startup(opCtx());

    std::vector<BSONObj> ops;
    ops.push_back(makeNoopOplogEntry(1));
    ops.push_back(makeNoopOplogEntry(2));

    size_t size = 16;
    buffer.push(opCtx(), ops.begin(), ops.end(), size);

    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(size, buffer.getSize());
    ASSERT_EQ(ops.size(), buffer.getCount());

    // Clear the buffer.
    buffer.clear(opCtx());

    // Clear should empty the buffer.
    ASSERT(buffer.isEmpty());
    ASSERT_EQ(0, buffer.getSize());
    ASSERT_EQ(0, buffer.getCount());

    // Clear should not prevent new data from being pushed.
    buffer.push(opCtx(), ops.begin(), ops.end(), size);

    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(size, buffer.getSize());
    ASSERT_EQ(ops.size(), buffer.getCount());

    // Shutdown the buffer.
    buffer.shutdown(opCtx());

    // Shutdown should empty the buffer.
    ASSERT(buffer.isEmpty());
    ASSERT_EQ(0, buffer.getSize());
    ASSERT_EQ(0, buffer.getCount());

    // Shutdown should prevent new data from being pushed.
    buffer.push(opCtx(), ops.begin(), ops.end(), size);

    ASSERT(buffer.isEmpty());
    ASSERT_EQ(0, buffer.getSize());
    ASSERT_EQ(0, buffer.getCount());
}

TEST_F(OplogBufferBatchedQueueTest, PushWaitForSpace) {
    OplogBufferBatchedQueue buffer(36);
    buffer.startup(opCtx());

    // Push first batch with 8 bytes.
    std::vector<BSONObj> ops1;
    ops1.push_back(makeNoopOplogEntry(1));
    ops1.push_back(makeNoopOplogEntry(2));

    size_t size1 = 8;
    buffer.push(opCtx(), ops1.begin(), ops1.end(), size1);

    // Push second batch with 16 bytes.
    std::vector<BSONObj> ops2;
    ops2.push_back(makeNoopOplogEntry(3));
    ops2.push_back(makeNoopOplogEntry(4));

    size_t size2 = 24;
    buffer.push(opCtx(), ops2.begin(), ops2.end(), size2);

    // Push third batch with 12 bytes, this should block.
    std::vector<BSONObj> ops3;
    ops3.push_back(makeNoopOplogEntry(5));
    ops3.push_back(makeNoopOplogEntry(6));

    size_t size3 = 16;
    stdx::thread pushThread([&, this] {
        // Will block until there is 12 bytes space in buffer.
        buffer.push(opCtx(), ops3.begin(), ops3.end(), size3);
    });

    // Wait for some time and check that the third batch has
    // not been pushed in.
    sleepsecs(3);
    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(size1 + size2, buffer.getSize());
    ASSERT_EQ(ops1.size() + ops2.size(), buffer.getCount());

    // Pop the first batch, wait for some time and check that
    // the third batch has still not been pushed in.
    OplogBatch<BSONObj> firstBatch;
    ASSERT_TRUE(buffer.tryPopBatch(opCtx(), &firstBatch));

    sleepsecs(3);
    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(size2, buffer.getSize());
    ASSERT_EQ(ops2.size(), buffer.getCount());

    // Pop the second batch, now there should be enough space
    // to push the third batch and unblock the pushThread.
    OplogBatch<BSONObj> secondBatch;
    ASSERT_TRUE(buffer.tryPopBatch(opCtx(), &secondBatch));

    pushThread.join();
    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(size3, buffer.getSize());
    ASSERT_EQ(ops3.size(), buffer.getCount());
}

TEST_F(OplogBufferBatchedQueueTest, shutdownUnblocksWaitForSpace) {
    OplogBufferBatchedQueue buffer(32);
    buffer.startup(opCtx());

    // Push first batch with 16 bytes.
    std::vector<BSONObj> ops1;
    ops1.push_back(makeNoopOplogEntry(1));
    ops1.push_back(makeNoopOplogEntry(2));

    size_t size1 = 16;
    buffer.push(opCtx(), ops1.begin(), ops1.end(), size1);

    // Push second batch with 24 bytes, this should block.
    std::vector<BSONObj> ops2;
    ops2.push_back(makeNoopOplogEntry(5));
    ops2.push_back(makeNoopOplogEntry(6));

    size_t size2 = 16;
    stdx::thread pushThread([&, this] {
        // Will block until there is 16 bytes space in buffer.
        buffer.push(opCtx(), ops2.begin(), ops2.end(), size2);
    });

    // Shutdown will clear the buffer and unblock the pushThread,
    // however it prevents the second batch from being pushed in.
    buffer.shutdown(opCtx());

    pushThread.join();
    ASSERT(buffer.isEmpty());
    ASSERT_EQ(0, buffer.getSize());
    ASSERT_EQ(0, buffer.getCount());
}

TEST_F(OplogBufferBatchedQueueTest, pushUnblocksWaitForData) {
    OplogBufferBatchedQueue buffer(32);
    buffer.startup(opCtx());

    OplogBatch<BSONObj> firstBatch;
    ASSERT_FALSE(buffer.tryPopBatch(opCtx(), &firstBatch));

    // The first waitForData should return false after waiting
    // since the buffer is still empty.
    ASSERT_FALSE(buffer.waitForData(Seconds(3)));

    // The second waitForData should return true after waiting
    // since there is new data pushed in.
    stdx::thread waitForDataThread([&, this] {
        // Will block until there data in buffer.
        ASSERT(buffer.waitForData(Seconds(3)));
    });

    std::vector<BSONObj> ops;
    ops.push_back(makeNoopOplogEntry(1));
    ops.push_back(makeNoopOplogEntry(2));

    size_t size = 16;
    buffer.push(opCtx(), ops.begin(), ops.end(), size);

    waitForDataThread.join();

    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(size, buffer.getSize());
    ASSERT_EQ(ops.size(), buffer.getCount());
}

TEST_F(OplogBufferBatchedQueueTest, shutdownUnblocksWaitForData) {
    OplogBufferBatchedQueue buffer(32);
    buffer.startup(opCtx());

    OplogBatch<BSONObj> firstBatch;
    ASSERT_FALSE(buffer.tryPopBatch(opCtx(), &firstBatch));

    // The first waitForData should return false after waiting
    // since the buffer is still empty.
    ASSERT_FALSE(buffer.waitForData(Seconds(3)));

    // Call waitForData with a long duration, shutdown should
    // unblock it and return false.
    stdx::thread waitForDataThread([&, this] {
        // Will block until there data in buffer.
        ASSERT_FALSE(buffer.waitForData(Days(1)));
    });

    buffer.shutdown(opCtx());
    waitForDataThread.join();

    ASSERT(buffer.isEmpty());
    ASSERT_EQ(0, buffer.getSize());
    ASSERT_EQ(0, buffer.getCount());
}

TEST_F(OplogBufferBatchedQueueTest, drainModeUnblocksWaitForData) {
    OplogBufferBatchedQueue buffer(32);
    buffer.startup(opCtx());

    OplogBatch<BSONObj> firstBatch;
    ASSERT_FALSE(buffer.tryPopBatch(opCtx(), &firstBatch));

    // The first waitForData should return false after waiting
    // since the buffer is still empty.
    ASSERT_FALSE(buffer.waitForData(Seconds(3)));

    // Call waitForData with a long duration, entering drain
    // mode should unblock it and return false.
    stdx::thread waitForDataThread1([&, this] {
        // Will block until there data in buffer.
        ASSERT_FALSE(buffer.waitForData(Days(1)));
    });

    buffer.enterDrainMode();
    waitForDataThread1.join();

    ASSERT(buffer.isEmpty());
    ASSERT_EQ(0, buffer.getSize());
    ASSERT_EQ(0, buffer.getCount());

    // Exit drain mode and check that waitForDataFor will block
    // indefinitely unless being interrupted.
    buffer.exitDrainMode();

    bool waitDone = false;
    stdx::thread waitForDataThread2([&, this] {
        try {
            // Will block until there data in buffer.
            ASSERT_FALSE(buffer.waitForDataFor(Days(1), opCtx()));
        } catch (ExceptionForCat<ErrorCategory::CancellationError>&) {
            std::cout << "WaitForData thread interrupted." << std::endl;
        }
        waitDone = true;
    });

    sleepsecs(3);
    ASSERT_FALSE(waitDone);

    opCtx()->markKilled(ErrorCodes::InterruptedAtShutdown);
    waitForDataThread2.join();

    ASSERT(waitDone);
}

}  // namespace repl
}  // namespace mongo
