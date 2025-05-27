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

#include "mongo/db/repl/oplog_buffer_blocking_queue.h"

#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {

namespace {

BSONObj makeNoopOplogEntry(int t) {
    auto oplogEntry = repl::DurableOplogEntry(
        OpTime(Timestamp(t, 1), 1),                                // optime
        OpTypeEnum::kNoop,                                         // opType
        NamespaceString::createNamespaceString_forTest("test.t"),  // namespace
        boost::none,                                               // uuid
        boost::none,                                               // fromMigrate
        boost::none,                                               // checkExistenceForDiffInsert
        boost::none,                                               // versionContext
        repl::OplogEntry::kOplogVersion,                           // version
        BSON("data" << t),                                         // o
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

BSONObj makeApplyOpsOplogEntry(int t, int num) {
    BSONObjBuilder oField;
    BSONArrayBuilder applyOps = oField.subarrayStart("applyOps");
    for (int i = 0; i < num; ++i) {
        applyOps.append(BSON("data" << i));
    }
    applyOps.doneFast();

    auto oplogEntry = repl::DurableOplogEntry(
        OpTime(Timestamp(t, 1), 1),                                // optime
        OpTypeEnum::kCommand,                                      // opType
        NamespaceString::createNamespaceString_forTest("test.t"),  // namespace
        boost::none,                                               // uuid
        boost::none,                                               // fromMigrate
        boost::none,                                               // checkExistenceForDiffInsert
        boost::none,                                               // versionContext
        repl::OplogEntry::kOplogVersion,                           // version
        oField.obj(),                                              // o
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

const std::size_t kOpSize = makeNoopOplogEntry(0).objsize();

}  // namespace

class OplogBufferBlockingQueueTest : public ServiceContextMongoDTest {
public:
    explicit OplogBufferBlockingQueueTest(Options options = {})
        : ServiceContextMongoDTest(options.useReplSettings(true)) {}

    void setUp() override;
    void tearDown() override;

    OperationContext* opCtx() const;

protected:
    ServiceContext* _serviceContext;
    ServiceContext::UniqueOperationContext _opCtxHolder;
};

void OplogBufferBlockingQueueTest::setUp() {
    ServiceContextMongoDTest::setUp();
    _serviceContext = getServiceContext();
    _opCtxHolder = makeOperationContext();
}

void OplogBufferBlockingQueueTest::tearDown() {
    _opCtxHolder = {};
    ServiceContextMongoDTest::tearDown();
}

OperationContext* OplogBufferBlockingQueueTest::opCtx() const {
    return _opCtxHolder.get();
}

TEST_F(OplogBufferBlockingQueueTest, PushEmptyBatch) {
    OplogBufferBlockingQueue buffer(1024 * 1024, 1024);
    buffer.startup(opCtx());

    std::vector<BSONObj> ops;
    buffer.push(opCtx(), ops.begin(), ops.end());

    ASSERT(buffer.isEmpty());
    ASSERT_EQ(0, buffer.getSize());
    ASSERT_EQ(0, buffer.getCount());

    BSONObj firstOp;
    ASSERT_FALSE(buffer.tryPop(opCtx(), &firstOp));
}

TEST_F(OplogBufferBlockingQueueTest, BasicBlockingQueueOperations) {
    OplogBufferBlockingQueue buffer(1024 * 1024, 1024);
    buffer.startup(opCtx());

    // Push initial one entry.
    std::vector<BSONObj> ops1;
    ops1.push_back(makeNoopOplogEntry(1));

    std::size_t size1 = kOpSize;
    buffer.push(opCtx(), ops1.begin(), ops1.end());

    // Verify the size and count.
    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(size1, buffer.getSize());
    ASSERT_EQ(ops1.size(), buffer.getCount());

    // Push some more entries.
    std::vector<BSONObj> ops2;
    ops2.push_back(makeNoopOplogEntry(2));
    ops2.push_back(makeNoopOplogEntry(3));

    buffer.push(opCtx(), ops2.begin(), ops2.end());

    // Verify the size and count.
    ASSERT(!buffer.isEmpty());
    std::size_t size2 = kOpSize * 2;
    ASSERT_EQ(size1 + size2, buffer.getSize());
    ASSERT_EQ(ops1.size() + ops2.size(), buffer.getCount());

    // Verify the first and last op.
    BSONObj firstOp;
    ASSERT(buffer.peek(opCtx(), &firstOp));
    ASSERT_BSONOBJ_EQ(ops1.front(), firstOp);

    auto lastOp = buffer.lastObjectPushed(opCtx());
    ASSERT(lastOp);
    ASSERT_BSONOBJ_EQ(ops2.back(), *lastOp);

    // Pop buffered ops, do one more and verify pop return false.
    BSONObj nextOp;
    ASSERT_TRUE(buffer.tryPop(opCtx(), &nextOp));
    ASSERT_TRUE(buffer.tryPop(opCtx(), &nextOp));
    ASSERT_TRUE(buffer.tryPop(opCtx(), &nextOp));

    ASSERT_FALSE(buffer.tryPop(opCtx(), &nextOp));
    ASSERT_FALSE(buffer.peek(opCtx(), &nextOp));
    ASSERT_BSONOBJ_EQ(ops2.back(), nextOp);

    // Verify the size and count.
    ASSERT(buffer.isEmpty());
    ASSERT_EQ(0, buffer.getSize());
    ASSERT_EQ(0, buffer.getCount());
}

TEST_F(OplogBufferBlockingQueueTest, ClearAndShutdownBuffer) {
    OplogBufferBlockingQueue buffer(1024 * 1024, 1024);
    buffer.startup(opCtx());

    std::vector<BSONObj> ops;
    ops.push_back(makeNoopOplogEntry(1));
    ops.push_back(makeNoopOplogEntry(2));

    std::size_t size = kOpSize * 2;
    buffer.push(opCtx(), ops.begin(), ops.end());

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
    buffer.push(opCtx(), ops.begin(), ops.end());

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
    buffer.push(opCtx(), ops.begin(), ops.end());

    ASSERT(buffer.isEmpty());
    ASSERT_EQ(0, buffer.getSize());
    ASSERT_EQ(0, buffer.getCount());
}

TEST_F(OplogBufferBlockingQueueTest, ShutdownNoClear) {
    OplogBufferBlockingQueue::Options options;
    options.clearOnShutdown = false;
    OplogBufferBlockingQueue buffer(1024 * 1024, 1024, nullptr, options);
    buffer.startup(opCtx());

    std::vector<BSONObj> ops1;
    ops1.push_back(makeNoopOplogEntry(1));
    ops1.push_back(makeNoopOplogEntry(2));

    std::size_t size1 = kOpSize * 2;
    buffer.push(opCtx(), ops1.begin(), ops1.end());

    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(size1, buffer.getSize());
    ASSERT_EQ(ops1.size(), buffer.getCount());

    // Shutdown the buffer.
    buffer.shutdown(opCtx());

    // Shutdown should not clear the buffer due to having
    // set the clearOnShutdown option.
    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(size1, buffer.getSize());
    ASSERT_EQ(ops1.size(), buffer.getCount());

    std::vector<BSONObj> ops2;
    ops2.push_back(makeNoopOplogEntry(3));

    // Shutdown should prevent new data from being pushed.
    buffer.push(opCtx(), ops2.begin(), ops2.end());

    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(size1, buffer.getSize());
    ASSERT_EQ(ops1.size(), buffer.getCount());
}

TEST_F(OplogBufferBlockingQueueTest, PushWaitForSpaceInBytes) {
    OplogBufferBlockingQueue buffer(kOpSize * 4, 1024);
    buffer.startup(opCtx());

    // Push 4 entries, the buffer should be full now.
    std::vector<BSONObj> ops1;
    ops1.push_back(makeNoopOplogEntry(1));
    ops1.push_back(makeNoopOplogEntry(2));
    ops1.push_back(makeNoopOplogEntry(3));
    ops1.push_back(makeNoopOplogEntry(4));

    std::size_t size1 = kOpSize * 4;
    buffer.push(opCtx(), ops1.begin(), ops1.end());

    // Push another 2 entries, this should block.
    std::vector<BSONObj> ops2;
    ops2.push_back(makeNoopOplogEntry(5));
    ops2.push_back(makeNoopOplogEntry(6));

    stdx::thread pushThread([&, this] {
        // Will block until there is space for 2 entries.
        buffer.push(opCtx(), ops2.begin(), ops2.end());
    });

    // Wait for some time and check that the second batch has
    // not been pushed in.
    sleepsecs(3);
    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(size1, buffer.getSize());
    ASSERT_EQ(ops1.size(), buffer.getCount());

    // Pop one entry, wait for some time and check that the
    // second batch has still not been pushed in.
    BSONObj nextOp;
    ASSERT_TRUE(buffer.tryPop(opCtx(), &nextOp));

    sleepsecs(3);
    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(size1 - kOpSize, buffer.getSize());
    ASSERT_EQ(ops1.size() - 1, buffer.getCount());

    // Pop another entry, now there should be enough space
    // to push the second batch and unblock the pushThread.
    ASSERT_TRUE(buffer.tryPop(opCtx(), &nextOp));

    pushThread.join();

    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(size1, buffer.getSize());
    ASSERT_EQ(ops1.size(), buffer.getCount());
}

TEST_F(OplogBufferBlockingQueueTest, PushSingleOpsWaitForSpaceInCount) {
    OplogBufferBlockingQueue buffer(1024 * 1024, 4);
    buffer.startup(opCtx());

    // Push 4 entries, the buffer should be full now.
    std::vector<BSONObj> ops1;
    ops1.push_back(makeNoopOplogEntry(1));
    ops1.push_back(makeNoopOplogEntry(2));
    ops1.push_back(makeNoopOplogEntry(3));
    ops1.push_back(makeNoopOplogEntry(4));

    std::size_t size1 = kOpSize * 4;
    buffer.push(opCtx(), ops1.begin(), ops1.end());

    // Push again with 2 entries, this should block.
    std::vector<BSONObj> ops2;
    ops2.push_back(makeNoopOplogEntry(5));
    ops2.push_back(makeNoopOplogEntry(6));

    stdx::thread pushThread([&, this] {
        // Will block until there is space for 2 entries.
        buffer.push(opCtx(), ops2.begin(), ops2.end());
    });

    // Wait for some time and check that the second batch has
    // not been pushed in.
    sleepsecs(3);
    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(size1, buffer.getSize());
    ASSERT_EQ(ops1.size(), buffer.getCount());

    // Pop one entry, wait for some time and check that the
    // second batch has still not been pushed in.
    BSONObj nextOp;
    ASSERT_TRUE(buffer.tryPop(opCtx(), &nextOp));

    sleepsecs(3);
    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(size1 - kOpSize, buffer.getSize());
    ASSERT_EQ(ops1.size() - 1, buffer.getCount());

    // Pop another entry, now there should be enough space
    // to push the second batch and unblock the pushThread.
    ASSERT_TRUE(buffer.tryPop(opCtx(), &nextOp));

    pushThread.join();
    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(size1, buffer.getSize());
    ASSERT_EQ(ops1.size(), buffer.getCount());
}

TEST_F(OplogBufferBlockingQueueTest, PushMultiOpsWaitForSpaceInCount) {
    OplogBufferBlockingQueue buffer(1024 * 1024, 10);
    buffer.startup(opCtx());

    // Push 3 entries with 10 ops, the buffer should be full now.
    std::vector<BSONObj> ops1;
    ops1.push_back(makeApplyOpsOplogEntry(1, 3));
    ops1.push_back(makeApplyOpsOplogEntry(2, 3));
    ops1.push_back(makeApplyOpsOplogEntry(3, 4));

    buffer.push(opCtx(), ops1.begin(), ops1.end());

    // Push another entry with 5 ops, this should block.
    std::vector<BSONObj> ops2;
    ops2.push_back(makeApplyOpsOplogEntry(4, 5));

    stdx::thread pushThread([&, this] {
        // Will block until there is space for 5 ops.
        buffer.push(opCtx(), ops2.begin(), ops2.end());
    });

    // Wait for some time and check that the second batch has
    // not been pushed in.
    sleepsecs(3);
    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(10, buffer.getCount());

    // Pop one entry, wait for some time and check that the
    // second batch has still not been pushed in.
    BSONObj nextOp;
    ASSERT_TRUE(buffer.tryPop(opCtx(), &nextOp));

    sleepsecs(3);
    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(7, buffer.getCount());

    // Pop another entry, now there should be enough space
    // to push the second batch and unblock the pushThread.
    ASSERT_TRUE(buffer.tryPop(opCtx(), &nextOp));

    pushThread.join();
    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(9, buffer.getCount());
}

TEST_F(OplogBufferBlockingQueueTest, PushAlwaysAllowOneEntryIfEmpty) {
    OplogBufferBlockingQueue buffer(1024 * 1024, 10);
    buffer.startup(opCtx());

    // Push 2 entries with the first one being larger than max count.
    // The first one should be allowed while the rest are blocked.
    std::vector<BSONObj> ops1;
    ops1.push_back(makeApplyOpsOplogEntry(1, 12));
    ops1.push_back(makeApplyOpsOplogEntry(2, 3));
    ops1.push_back(makeApplyOpsOplogEntry(3, 4));

    bool doneWait = false;
    stdx::thread pushThread([&, this] {
        // Will block until the first large entry is popped.
        buffer.push(opCtx(), ops1.begin(), ops1.end());
        doneWait = true;
    });

    // Wait for some time and check only the first entry is pushed.
    sleepsecs(3);
    ASSERT(!doneWait);
    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(12, buffer.getCount());

    // Pop the first entry, now there should be enough space to push
    // the rest of the entries in the batch.
    BSONObj nextOp;
    ASSERT_TRUE(buffer.tryPop(opCtx(), &nextOp));

    pushThread.join();
    ASSERT(doneWait);
    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(7, buffer.getCount());
}

TEST_F(OplogBufferBlockingQueueTest, PushAlwaysAllowOneEntryIfEmpty2) {
    OplogBufferBlockingQueue buffer(1024 * 1024, 10);
    buffer.startup(opCtx());

    // Push one entry with 2 ops, this should make the queue non-empty.
    std::vector<BSONObj> ops1;
    ops1.push_back(makeApplyOpsOplogEntry(1, 2));

    buffer.push(opCtx(), ops1.begin(), ops1.end());

    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(2, buffer.getCount());

    // Push another 3 entries with the first one being larger than max
    // count. Since the buffer is not empty, this should be blocked.
    std::vector<BSONObj> ops2;
    ops2.push_back(makeApplyOpsOplogEntry(2, 12));
    ops2.push_back(makeApplyOpsOplogEntry(3, 3));
    ops2.push_back(makeApplyOpsOplogEntry(4, 4));

    bool doneWait = false;
    stdx::thread pushThread([&, this] {
        // Will block until the first large entry is popped.
        buffer.push(opCtx(), ops2.begin(), ops2.end());
        doneWait = true;
    });

    // Wait for some time and check that the first entry has not been
    // pushed in.
    sleepsecs(3);
    ASSERT(!doneWait);
    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(2, buffer.getCount());

    // Pop the initial entry, now the buffer is empty, so the first
    // entry in the batch should be allowed to get in even though
    // it is larger than the max count.
    BSONObj nextOp;
    ASSERT_TRUE(buffer.tryPop(opCtx(), &nextOp));

    // Wait for some time and check the first entry in the batch is
    // pushed in.
    sleepsecs(3);
    ASSERT(!doneWait);
    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(12, buffer.getCount());

    // Pop the first entry, now there should be enough space to push
    // the rest of the entries in the batch.
    ASSERT_TRUE(buffer.tryPop(opCtx(), &nextOp));

    pushThread.join();
    ASSERT(doneWait);
    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(7, buffer.getCount());
}

TEST_F(OplogBufferBlockingQueueTest, shutdownUnblocksWaitForSpace) {
    OplogBufferBlockingQueue buffer(kOpSize * 4, 1024);
    buffer.startup(opCtx());

    // Push 3 entries.
    std::vector<BSONObj> ops1;
    ops1.push_back(makeNoopOplogEntry(1));
    ops1.push_back(makeNoopOplogEntry(2));
    ops1.push_back(makeNoopOplogEntry(3));

    buffer.push(opCtx(), ops1.begin(), ops1.end());

    // Push another 2 entries, this should block.
    std::vector<BSONObj> ops2;
    ops2.push_back(makeNoopOplogEntry(4));
    ops2.push_back(makeNoopOplogEntry(5));

    stdx::thread pushThread([&, this] {
        // Will block until there is space for 2 entries.
        buffer.push(opCtx(), ops2.begin(), ops2.end());
    });

    // Shutdown will clear the buffer and unblock the pushThread,
    // however it prevents the second batch from being pushed in.
    buffer.shutdown(opCtx());

    pushThread.join();
    ASSERT(buffer.isEmpty());
    ASSERT_EQ(0, buffer.getSize());
    ASSERT_EQ(0, buffer.getCount());
}

TEST_F(OplogBufferBlockingQueueTest, pushUnblocksWaitForData) {
    OplogBufferBlockingQueue buffer(kOpSize * 3, 1024);
    buffer.startup(opCtx());

    BSONObj firstOp;
    ASSERT_FALSE(buffer.tryPop(opCtx(), &firstOp));

    // The first waitForData should return false after waiting
    // since the buffer is still empty.
    ASSERT_FALSE(buffer.waitForData(Seconds(3)));

    // Call waitForData with a long duration, push should
    // unblock it and return true.
    stdx::thread waitForDataThread([&, this] {
        // Will block until there is data in buffer.
        ASSERT(buffer.waitForData(Days(1)));
    });

    std::vector<BSONObj> ops;
    ops.push_back(makeNoopOplogEntry(1));
    ops.push_back(makeNoopOplogEntry(2));

    std::size_t size = kOpSize * 2;
    buffer.push(opCtx(), ops.begin(), ops.end());

    waitForDataThread.join();

    ASSERT(!buffer.isEmpty());
    ASSERT_EQ(size, buffer.getSize());
    ASSERT_EQ(ops.size(), buffer.getCount());
}

TEST_F(OplogBufferBlockingQueueTest, shutdownUnblocksWaitForData) {
    OplogBufferBlockingQueue buffer(kOpSize * 3, 1024);
    buffer.startup(opCtx());

    BSONObj firstOp;
    ASSERT_FALSE(buffer.tryPop(opCtx(), &firstOp));

    // The first waitForData should return false after waiting
    // since the buffer is still empty.
    ASSERT_FALSE(buffer.waitForData(Seconds(3)));

    // Call waitForData with a long duration, shutdown should
    // unblock it and return false.
    stdx::thread waitForDataThread([&, this] {
        // Will block until there is data in buffer.
        ASSERT_FALSE(buffer.waitForData(Days(1)));
    });

    buffer.shutdown(opCtx());
    waitForDataThread.join();

    ASSERT(buffer.isEmpty());
    ASSERT_EQ(0, buffer.getSize());
    ASSERT_EQ(0, buffer.getCount());
}

TEST_F(OplogBufferBlockingQueueTest, drainModeUnblocksWaitForData) {
    OplogBufferBlockingQueue buffer(kOpSize * 3, 1024);
    buffer.startup(opCtx());

    BSONObj firstOp;
    ASSERT_FALSE(buffer.tryPop(opCtx(), &firstOp));

    // The first waitForData should return false after waiting
    // since the buffer is still empty.
    ASSERT_FALSE(buffer.waitForData(Seconds(3)));

    // Call waitForData with a long duration, entering drain
    // mode should unblock it and return false.
    stdx::thread waitForDataThread1([&, this] {
        // Will block until there is data in buffer.
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

    bool doneWait = false;
    stdx::thread waitForDataThread2([&, this] {
        // Will block until there data in buffer.
        ASSERT_THROWS_CODE(buffer.waitForDataFor(Days(1), opCtx()),
                           DBException,
                           ErrorCodes::InterruptedAtShutdown);
        doneWait = true;
    });

    sleepsecs(3);
    ASSERT(!doneWait);

    opCtx()->markKilled(ErrorCodes::InterruptedAtShutdown);
    waitForDataThread2.join();

    ASSERT(doneWait);
}

}  // namespace repl
}  // namespace mongo
