/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/persistent_task_queue.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString("test", "foo");

struct TestTask {
    std::string key;
    int val;

    TestTask() : val(0) {}
    TestTask(std::string key, int val) : key(std::move(key)), val(val) {}
    TestTask(BSONObj bson)
        : key(bson.getField("key").String()), val(bson.getField("value").Int()) {}

    static TestTask parse(IDLParserContext, BSONObj bson) {
        return TestTask{bson};
    }

    void serialize(BSONObjBuilder& builder) const {
        builder.append("key", key);
        builder.append("value", val);
    }

    BSONObj toBSON() const {
        BSONObjBuilder builder;
        serialize(builder);
        return builder.obj();
    }
};

void killOps(ServiceContext* serviceCtx) {
    ServiceContext::LockedClientsCursor cursor(serviceCtx);

    for (Client* client = cursor.next(); client != nullptr; client = cursor.next()) {
        stdx::lock_guard<Client> lk(*client);
        if (client->isFromSystemConnection() && !client->canKillSystemOperationInStepdown(lk))
            continue;

        OperationContext* toKill = client->getOperationContext();

        if (toKill && !toKill->isKillPending())
            serviceCtx->killOperation(lk, toKill, ErrorCodes::Interrupted);
    }
}

class PersistentTaskQueueTest : public ShardServerTestFixture {
    void setUp() override {
        ShardServerTestFixture::setUp();
        AutoGetDb autoDb(operationContext(), kNss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(operationContext(), kNss, MODE_IX);
        CollectionShardingRuntime::get(operationContext(), kNss)
            ->setFilteringMetadata(operationContext(), CollectionMetadata());
    }
};

// Test that writes to the queue persist across instantiations.
TEST_F(PersistentTaskQueueTest, TestWritesPersistInstances) {

    auto opCtx = operationContext();

    {
        PersistentTaskQueue<TestTask> q(opCtx, kNss);

        ASSERT_EQ(q.size(opCtx), 0UL);
        ASSERT_TRUE(q.empty(opCtx));

        q.push(opCtx, {"age", 5});

        ASSERT_EQ(q.size(opCtx), 1UL);
        ASSERT_EQ(q.peek(opCtx).task.key, "age");
        ASSERT_EQ(q.peek(opCtx).task.val, 5);
        ASSERT_FALSE(q.empty(opCtx));
    }

    {
        PersistentTaskQueue<TestTask> q(opCtx, kNss);

        ASSERT_EQ(q.size(opCtx), 1UL);
        ASSERT_EQ(q.peek(opCtx).task.key, "age");
        ASSERT_EQ(q.peek(opCtx).task.val, 5);
        ASSERT_FALSE(q.empty(opCtx));

        q.pop(opCtx);
        ASSERT_EQ(q.size(opCtx), 0UL);
        ASSERT_TRUE(q.empty(opCtx));
    }

    {
        PersistentTaskQueue<TestTask> q(opCtx, kNss);

        ASSERT_EQ(q.size(opCtx), 0UL);
        ASSERT_TRUE(q.empty(opCtx));
    }
}

// Test that the FIFO order of elements is preserved across instances.
TEST_F(PersistentTaskQueueTest, TestFIFOPreservedAcrossInstances) {
    auto opCtx = operationContext();

    {
        PersistentTaskQueue<TestTask> q(opCtx, kNss);

        ASSERT_EQ(q.size(opCtx), 0UL);
        ASSERT_TRUE(q.empty(opCtx));

        for (int i = 5; i < 9; ++i) {
            q.push(opCtx, TestTask{"age", i});
        }

        ASSERT_EQ(q.size(opCtx), 4UL);
        ASSERT_EQ(q.peek(opCtx).task.val, 5);
        ASSERT_FALSE(q.empty(opCtx));
    }

    {
        PersistentTaskQueue<TestTask> q(opCtx, kNss);

        ASSERT_EQ(q.size(opCtx), 4UL);
        ASSERT_FALSE(q.empty(opCtx));

        for (int i = 5; i < 9; ++i) {
            ASSERT_EQ(q.size(opCtx), 9UL - i);

            auto cur = q.peek(opCtx);
            ASSERT_EQ(cur.task.key, "age");
            ASSERT_EQ(cur.task.val, i);
            ASSERT_EQ(cur.id, i - 4LL);
            q.pop(opCtx);
        }

        ASSERT_EQ(q.size(opCtx), 0UL);
        ASSERT_TRUE(q.empty(opCtx));
    }
}

// Test that ids are sequential across intances when items are in db.
TEST_F(PersistentTaskQueueTest, TestIdIsContinueAcrossInstances) {
    auto opCtx = operationContext();

    {
        PersistentTaskQueue<TestTask> q(opCtx, kNss);

        ASSERT_EQ(q.size(opCtx), 0UL);
        ASSERT_TRUE(q.empty(opCtx));

        auto id = q.push(opCtx, {"age", 5});
        ASSERT_EQ(id, 1LL);

        auto cur = q.peek(opCtx);

        ASSERT_EQ(q.size(opCtx), 1UL);
        ASSERT_EQ(cur.task.key, "age");
        ASSERT_EQ(cur.task.val, 5);
        ASSERT_EQ(id, cur.id);
        ASSERT_FALSE(q.empty(opCtx));
    }

    {
        PersistentTaskQueue<TestTask> q(opCtx, kNss);

        ASSERT_EQ(q.size(opCtx), 1UL);
        ASSERT_EQ(q.peek(opCtx).task.val, 5);
        ASSERT_EQ(q.peek(opCtx).id, 1LL);
        ASSERT_FALSE(q.empty(opCtx));

        q.pop(opCtx);
        ASSERT_EQ(q.size(opCtx), 0UL);
        ASSERT_TRUE(q.empty(opCtx));

        auto id = q.push(opCtx, {"age", 5});
        ASSERT_EQ(id, 2LL);

        auto cur = q.peek(opCtx);

        ASSERT_EQ(q.size(opCtx), 1UL);
        ASSERT_EQ(cur.task.key, "age");
        ASSERT_EQ(cur.task.val, 5);
        ASSERT_EQ(id, cur.id);
        ASSERT_FALSE(q.empty(opCtx));
    }

    {
        PersistentTaskQueue<TestTask> q(opCtx, kNss);

        ASSERT_EQ(q.size(opCtx), 1UL);
        ASSERT_FALSE(q.empty(opCtx));
    }
}

// Test interrupting blocking peek call before it starts waiting on the condition variable.
TEST_F(PersistentTaskQueueTest, TestInterruptedBeforeWaitingOnCV) {
    auto opCtx = operationContext();
    PersistentTaskQueue<TestTask> q(opCtx, kNss);

    // Set interrupted state before waiting on condition variable.
    q.close(opCtx);

    // Assert that wakeup is not lost.
    ASSERT_THROWS(q.peek(opCtx), ExceptionFor<ErrorCodes::Interrupted>);
}

// Test wakeup from wait on empty queue.
TEST_F(PersistentTaskQueueTest, TestWakeupOnEmptyQueue) {
    auto opCtx = operationContext();
    PersistentTaskQueue<TestTask> q(opCtx, kNss);

    auto result = stdx::async(stdx::launch::async, [&q] {
        ThreadClient tc("RangeDeletionService", getGlobalServiceContext());
        auto opCtx = tc->makeOperationContext();

        stdx::this_thread::sleep_for(stdx::chrono::milliseconds(500));
        q.push(opCtx.get(), {"age", 5});
    });

    ASSERT_EQ(q.peek(opCtx).task.val, 5);
    ASSERT_EQ(q.size(opCtx), 1UL);
    ASSERT_FALSE(q.empty(opCtx));
}

// Test interrupting blocking peek call after it starts waiting on the condition variable.
TEST_F(PersistentTaskQueueTest, TestInterruptedWhileWaitingOnCV) {
    auto opCtx = operationContext();
    PersistentTaskQueue<TestTask> q(opCtx, kNss);

    unittest::Barrier barrier(2);

    auto result = stdx::async(stdx::launch::async, [opCtx, &q, &barrier] {
        ThreadClient tc("RangeDeletionService", getGlobalServiceContext());
        auto opCtx = tc->makeOperationContext();

        barrier.countDownAndWait();
        q.peek(opCtx.get());
    });

    // Sleeps a little to make sure the thread calling peek has a chance to reach the condition
    // variable.
    barrier.countDownAndWait();
    stdx::this_thread::sleep_for(stdx::chrono::milliseconds(100));
    q.close(opCtx);

    ASSERT_THROWS(result.get(), ExceptionFor<ErrorCodes::Interrupted>);
}

// Test that waiting on the condition variable is interrupted when the operation context is killed.
TEST_F(PersistentTaskQueueTest, TestKilledOperationContextWhileWaitingOnCV) {
    auto opCtx = operationContext();
    PersistentTaskQueue<TestTask> q(opCtx, kNss);

    unittest::Barrier barrier(2);

    auto result = stdx::async(stdx::launch::async, [opCtx, &q, &barrier] {
        ThreadClient tc("RangeDeletionService", getGlobalServiceContext());
        {
            stdx::lock_guard<Client> lk(*tc.get());
            tc->setSystemOperationKillableByStepdown(lk);
        }

        auto opCtx = tc->makeOperationContext();

        barrier.countDownAndWait();
        q.peek(opCtx.get());
    });

    // Sleeps a little to make sure the thread calling peek has a chance to reach the condition
    // variable.
    barrier.countDownAndWait();
    stdx::this_thread::sleep_for(stdx::chrono::milliseconds(100));
    killOps(getServiceContext());

    ASSERT_THROWS(result.get(), ExceptionFor<ErrorCodes::Interrupted>);
}

// Test that pop throws if peek is not called.
TEST_F(PersistentTaskQueueTest, TestPopThrowsIfPeekNotCalled) {
    auto opCtx = operationContext();
    PersistentTaskQueue<TestTask> q(opCtx, kNss);

    ASSERT_THROWS(q.pop(opCtx), std::exception);
}

}  // namespace
}  // namespace mongo
