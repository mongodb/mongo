
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/free_mon/free_mon_message.h"
#include "mongo/db/free_mon/free_mon_queue.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

class FreeMonQueueTest : public ServiceContextMongoDTest {
private:
    void setUp() final;
    void tearDown() final;

protected:
    ServiceContext::UniqueOperationContext _opCtx;

    executor::NetworkInterfaceMock* _mockNetwork{nullptr};

    std::unique_ptr<executor::ThreadPoolTaskExecutor> _mockThreadPool;
};

void FreeMonQueueTest::setUp() {
    ServiceContextMongoDTest::setUp();

    // Set up a NetworkInterfaceMock. Note, unlike NetworkInterfaceASIO, which has its own pool of
    // threads, tasks in the NetworkInterfaceMock must be carried out synchronously by the (single)
    // thread the unit test is running on.
    auto netForFixedTaskExecutor = std::make_unique<executor::NetworkInterfaceMock>();
    _mockNetwork = netForFixedTaskExecutor.get();

    // Set up a ThreadPoolTaskExecutor. Note, for local tasks this TaskExecutor uses a
    // ThreadPoolMock, and for remote tasks it uses the NetworkInterfaceMock created above. However,
    // note that the ThreadPoolMock uses the NetworkInterfaceMock's threads to run tasks, which is
    // again just the (single) thread the unit test is running on. Therefore, all tasks, local and
    // remote, must be carried out synchronously by the test thread.
    _mockThreadPool = makeThreadPoolTestExecutor(std::move(netForFixedTaskExecutor));

    _mockThreadPool->startup();

    _opCtx = cc().makeOperationContext();
}

void FreeMonQueueTest::tearDown() {
    _opCtx = {};

    ServiceContextMongoDTest::tearDown();
}

// Postive: Can we enqueue and dequeue one item
TEST_F(FreeMonQueueTest, TestBasic) {
    FreeMonMessageQueue queue;

    queue.enqueue(FreeMonMessage::createNow(FreeMonMessageType::RegisterServer));

    auto item = queue.dequeue(_opCtx.get()->getServiceContext()->getPreciseClockSource());

    ASSERT(item.get()->getType() == FreeMonMessageType::RegisterServer);
}

Date_t fromNow(int millis) {
    return getGlobalServiceContext()->getPreciseClockSource()->now() + Milliseconds(millis);
}

// Positive: Ensure deadlines sort properly
TEST_F(FreeMonQueueTest, TestDeadlinePriority) {
    FreeMonMessageQueue queue;

    queue.enqueue(
        FreeMonMessage::createWithDeadline(FreeMonMessageType::RegisterServer, fromNow(5000)));
    queue.enqueue(
        FreeMonMessage::createWithDeadline(FreeMonMessageType::RegisterCommand, fromNow(50)));

    auto item = queue.dequeue(_opCtx.get()->getServiceContext()->getPreciseClockSource()).get();
    ASSERT(item->getType() == FreeMonMessageType::RegisterCommand);

    item = queue.dequeue(_opCtx.get()->getServiceContext()->getPreciseClockSource()).get();
    ASSERT(item->getType() == FreeMonMessageType::RegisterServer);
}

// Positive: Ensure deadlines sort properly when they have the same deadlines
TEST_F(FreeMonQueueTest, TestFIFO) {
    FreeMonMessageQueue queue;

    queue.enqueue(FreeMonMessage::createWithDeadline(FreeMonMessageType::RegisterServer, Date_t()));
    queue.enqueue(
        FreeMonMessage::createWithDeadline(FreeMonMessageType::AsyncRegisterComplete, Date_t()));
    queue.enqueue(
        FreeMonMessage::createWithDeadline(FreeMonMessageType::RegisterCommand, Date_t()));

    auto item = queue.dequeue(_opCtx.get()->getServiceContext()->getPreciseClockSource()).get();
    ASSERT(item->getType() == FreeMonMessageType::RegisterServer);

    item = queue.dequeue(_opCtx.get()->getServiceContext()->getPreciseClockSource()).get();
    ASSERT(item->getType() == FreeMonMessageType::AsyncRegisterComplete);

    item = queue.dequeue(_opCtx.get()->getServiceContext()->getPreciseClockSource()).get();
    ASSERT(item->getType() == FreeMonMessageType::RegisterCommand);
}


// Positive: Test Queue Stop
TEST_F(FreeMonQueueTest, TestQueueStop) {
    FreeMonMessageQueue queue;

    queue.enqueue(
        FreeMonMessage::createWithDeadline(FreeMonMessageType::RegisterServer, fromNow(50000)));

    unittest::Barrier barrier(2);

    auto swSchedule =
        _mockThreadPool->scheduleWork([&](const executor::TaskExecutor::CallbackArgs& cbArgs) {

            barrier.countDownAndWait();

            // Try to dequeue from a stopped task queue
            auto item = queue.dequeue(_opCtx.get()->getServiceContext()->getPreciseClockSource());
            ASSERT_FALSE(item.is_initialized());

        });

    ASSERT_OK(swSchedule.getStatus());

    // Stop the queue
    queue.stop();

    // Let our worker thread proceed
    barrier.countDownAndWait();

    _mockThreadPool->shutdown();
    _mockThreadPool->join();
}

}  // namespace
}  // namespace mongo
