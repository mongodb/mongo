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
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

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

    void push(OperationContext*,
              Batch::const_iterator begin,
              Batch::const_iterator end,
              boost::optional<std::size_t> bytes) {
        MONGO_UNIMPLEMENTED;
    }

    void push_forTest(OplogWriterBatch& batch) {
        stdx::unique_lock<Latch> lk(_mutex);
        _queue.push(batch);
        _notEmptyCv.notify_one();
    }

    void waitForSpace(OperationContext*, std::size_t size) {
        MONGO_UNIMPLEMENTED;
    }

    bool isEmpty() const {
        stdx::lock_guard<Latch> lk(_mutex);
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

    bool tryPopBatch(OperationContext* opCtx, OplogBatch<Value>* batch) {
        stdx::lock_guard<Latch> lk(_mutex);
        if (_queue.empty()) {
            return false;
        }
        *batch = std::move(_queue.front());
        _queue.pop();
        return true;
    }

    bool waitForDataFor(Milliseconds waitDuration, Interruptible* interruptible) {
        stdx::unique_lock<Latch> lk(_mutex);
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

    void enterDrainMode() {
        stdx::lock_guard<Latch> lk(_mutex);
        _drainMode = true;
    }

    void exitDrainMode() {
        stdx::lock_guard<Latch> lk(_mutex);
        _drainMode = false;
    }

    bool inDrainMode() {
        stdx::lock_guard<Latch> lk(_mutex);
        return _drainMode;
    }

    mutable Mutex _mutex = MONGO_MAKE_LATCH("OplogWriterBufferMock::mutex");
    stdx::condition_variable _notEmptyCv;
    std::queue<OplogWriterBatch> _queue;
    bool _drainMode = false;
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

    ReplicationCoordinator* getReplCoord() const;
    OperationContext* opCtx() const;

protected:
    ServiceContext* _serviceContext;
    ServiceContext::UniqueOperationContext _opCtxHolder;
};

void OplogWriterBatcherTest::setUp() {
    ServiceContextMongoDTest::setUp();

    _serviceContext = getServiceContext();
    _opCtxHolder = makeOperationContext();
    ReplicationCoordinator::set(_serviceContext,
                                std::make_unique<ReplicationCoordinatorMock>(_serviceContext));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
}

void OplogWriterBatcherTest::tearDown() {
    _opCtxHolder = {};
    ServiceContextMongoDTest::tearDown();
}

ReplicationCoordinator* OplogWriterBatcherTest::getReplCoord() const {
    return ReplicationCoordinator::get(_serviceContext);
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

    OplogWriterBatch batch1({makeNoopOplogEntry(Seconds(123)), makeNoopOplogEntry(Seconds(456))},
                            2);
    OplogWriterBatch batch2({makeNoopOplogEntry(Seconds(789))}, 1);
    writerBuffer.push_forTest(batch1);
    writerBuffer.push_forTest(batch2);

    auto batch = writerBatcher.getNextBatch(opCtx(), Seconds(1)).releaseBatch();
    ASSERT_EQ(3, batch.size());
}

TEST_F(OplogWriterBatcherTest, WriterBatchSizeInBytes) {
    OplogWriterBufferMock writerBuffer;
    OplogWriterBatcher writerBatcher(&writerBuffer);

    OplogWriterBatch batch1({makeNoopOplogEntry(Seconds(123))}, 16 * 1024 * 1024 /*16MB*/);
    OplogWriterBatch batch2({makeNoopOplogEntry(Seconds(123))}, 1);
    OplogWriterBatch batch3({makeNoopOplogEntry(Seconds(123))}, 1);
    OplogWriterBatch batch4({makeNoopOplogEntry(Seconds(123))}, 16 * 1024 * 1024 /*16MB*/);
    writerBuffer.push_forTest(batch1);
    writerBuffer.push_forTest(batch2);
    writerBuffer.push_forTest(batch3);
    writerBuffer.push_forTest(batch4);

    // Once we can form a batch whose bytes size in (16MB, 32MB], we return immediately.
    ASSERT_EQ(16 * 1024 * 1024 + 1, writerBatcher.getNextBatch(opCtx(), Seconds(1)).byteSize());
    ASSERT_EQ(16 * 1024 * 1024 + 1, writerBatcher.getNextBatch(opCtx(), Seconds(1)).byteSize());
    ASSERT_TRUE(writerBatcher.getNextBatch(opCtx(), Seconds(1)).empty());
}

DEATH_TEST_F(OplogWriterBatcherTest, BatchFromBufferSizeLimit, "batchSize <= kMinWriterBatchSize") {
    OplogWriterBufferMock writerBuffer;
    OplogWriterBatcher writerBatcher(&writerBuffer);

    OplogWriterBatch batch1({makeNoopOplogEntry(Seconds(123))}, 16 * 1024 * 1024 /*16MB*/ + 1);
    writerBuffer.push_forTest(batch1);

    writerBatcher.getNextBatch(opCtx(), Seconds(1));
}

TEST_F(OplogWriterBatcherTest, BatcherWillReturnNewBatchInBufferWhenNoDataAvailable) {
    OplogWriterBufferMock writerBuffer;
    OplogWriterBatcher writerBatcher(&writerBuffer);

    OplogWriterBatch batch1({makeNoopOplogEntry(Seconds(123))}, 16 * 1024 * 1024 /*16MB*/);

    stdx::thread pushBufferThread([&]() {
        Client::initThread("pushBufferThread", getGlobalServiceContext()->getService());
        // Wait 1s so the batcher can pop batch1 and wait more data.
        sleepsecs(1);
        writerBuffer.push_forTest(batch1);
    });

    // Make the maxWaitTime longer to give another thread enough time to push data into buffer.
    auto batch = writerBatcher.getNextBatch(opCtx(), Seconds(10));

    pushBufferThread.join();

    ASSERT_EQ(16 * 1024 * 1024, batch.byteSize());
}

TEST_F(OplogWriterBatcherTest, BatcherWillNotWaitForMoreDataWhenAlreadyHaveBatch) {
    OplogWriterBufferMock writerBuffer;
    OplogWriterBatcher writerBatcher(&writerBuffer);

    OplogWriterBatch batch1({makeNoopOplogEntry(Seconds(123))}, 16 * 1024 * 1024 /*16MB*/);
    OplogWriterBatch batch2({makeNoopOplogEntry(Seconds(123))}, 16 * 1024 * 1024 /*16MB*/);
    writerBuffer.push_forTest(batch1);

    stdx::thread pushBufferThread([&]() {
        Client::initThread("pushBufferThread", getGlobalServiceContext()->getService());
        // Wait 5s so the other thread can read the first batch and return.
        sleepsecs(5);
        writerBuffer.push_forTest(batch2);
    });

    // Make the maxWaitTime longer to give another thread enough time to push data into buffer.
    auto batch = writerBatcher.getNextBatch(opCtx(), Seconds(10));

    pushBufferThread.join();

    // The batcher should not wait for the batch pushed by pushBufferThread.
    ASSERT_EQ(16 * 1024 * 1024, batch.byteSize());
}

TEST_F(OplogWriterBatcherTest, BatcherWaitSecondaryDelaySecs) {
    OplogWriterBufferMock writerBuffer;
    OplogWriterBatcher writerBatcher(&writerBuffer);

    // Set SecondaryDelaySecs to a large number, so we should get empty batches at the beginning.
    dynamic_cast<ReplicationCoordinatorMock*>(getReplCoord())->setSecondaryDelaySecs(Seconds(500));
    auto startTime = durationCount<Seconds>(Date_t::now().toDurationSinceEpoch());

    // Put one entry that is over secondaryDelaySecs and one entry not, the batcher will wait until
    // all entries pass secondaryDelaySecs to return.
    OplogWriterBatch batch1(
        {makeNoopOplogEntry(Seconds(startTime - 100)), makeNoopOplogEntry(Seconds(startTime))},
        16 * 1024 * 1024 /*16MB*/);
    writerBuffer.push_forTest(batch1);

    for (auto i = 0; i < 5; i++) {
        ASSERT_TRUE(writerBatcher.getNextBatch(opCtx(), Seconds(1)).empty());
    }

    // Set SecondaryDelaySecs to a small number, then we can get the batch.
    dynamic_cast<ReplicationCoordinatorMock*>(getReplCoord())->setSecondaryDelaySecs(Seconds(5));

    auto batch = writerBatcher.getNextBatch(opCtx(), Seconds(1));
    auto endTime = durationCount<Seconds>(Date_t::now().toDurationSinceEpoch());
    ASSERT_TRUE(endTime - startTime >= 5);
}

TEST_F(OplogWriterBatcherTest, BatcherWaitSecondaryDelaySecsReturnFirstBatch) {
    OplogWriterBufferMock writerBuffer;
    OplogWriterBatcher writerBatcher(&writerBuffer);

    dynamic_cast<ReplicationCoordinatorMock*>(getReplCoord())->setSecondaryDelaySecs(Seconds(5));
    auto startTime = durationCount<Seconds>(Date_t::now().toDurationSinceEpoch());

    // If we have two batches from the buffer that can be merged into one and the second batch
    // doesn't meet secondaryDelaySecs, we should return the first batch immediately.
    OplogWriterBatch batch1({makeNoopOplogEntry(Seconds(startTime - 100))},
                            16 * 1024 * 1024 /*16MB*/);
    OplogWriterBatch batch2({makeNoopOplogEntry(Seconds(startTime))}, 16 * 1024 * 1024 /*16MB*/);
    writerBuffer.push_forTest(batch1);
    writerBuffer.push_forTest(batch2);

    auto batch = writerBatcher.getNextBatch(opCtx(), Seconds(1));
    ASSERT_EQ(16 * 1024 * 1024, batch.byteSize());
}

TEST_F(OplogWriterBatcherTest, BatcherCheckDraining) {
    OplogWriterBufferMock writerBuffer;
    OplogWriterBatcher writerBatcher(&writerBuffer);
    writerBuffer.enterDrainMode();

    // Keep polling from the batcher, we should get an empty draining batch every time and wait 1s
    // inside.
    for (auto i = 0; i < 5; i++) {
        auto startTime = durationCount<Seconds>(Date_t::now().toDurationSinceEpoch());
        auto drainedBatch = writerBatcher.getNextBatch(opCtx(), Seconds(1));
        ASSERT_TRUE(drainedBatch.exhausted());
        auto endTime = durationCount<Seconds>(Date_t::now().toDurationSinceEpoch());
        ASSERT_TRUE(endTime - startTime >= 1);
    }

    // Exit drain mode and the batcher should be able to get the batch from the buffer.
    writerBuffer.exitDrainMode();
    OplogWriterBatch batch1({makeNoopOplogEntry(Seconds(123))}, 16 * 1024 * 1024 /*16MB*/);
    writerBuffer.push_forTest(batch1);
    auto batch = writerBatcher.getNextBatch(opCtx(), Days(1));
    ASSERT_FALSE(batch.exhausted());
    ASSERT_EQ(16 * 1024 * 1024, batch.byteSize());

    // Draining again, should still work.
    writerBuffer.enterDrainMode();
    for (auto i = 0; i < 5; i++) {
        auto startTime = durationCount<Seconds>(Date_t::now().toDurationSinceEpoch());
        auto drainedBatch = writerBatcher.getNextBatch(opCtx(), Seconds(1));
        ASSERT_TRUE(drainedBatch.exhausted());
        auto endTime = durationCount<Seconds>(Date_t::now().toDurationSinceEpoch());
        ASSERT_TRUE(endTime - startTime >= 1);
    }
}

}  // namespace repl
}  // namespace mongo
