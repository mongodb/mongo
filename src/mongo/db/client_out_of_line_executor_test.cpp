/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <memory>

#include "mongo/db/client.h"
#include "mongo/db/client_out_of_line_executor.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

/**
 * The following implements a pseudo garbage collector to test client's out-of-line executor.
 */
class ClientOutOfLineExecutorTest : public unittest::Test {
public:
    class DummyInstance {
    public:
        DummyInstance() = delete;

        DummyInstance(ClientOutOfLineExecutorTest* parent) : _parent(parent) {
            _parent->_instanceCount.fetchAndAdd(1);
        }

        DummyInstance(DummyInstance&& other) : _parent(std::move(other._parent)) {
            _parent->_instanceCount.fetchAndAdd(1);
        }

        DummyInstance(const DummyInstance& other) {
            MONGO_UNREACHABLE;
        }

        ~DummyInstance() {
            _parent->_instanceCount.fetchAndAdd(-1);
        }

    private:
        ClientOutOfLineExecutorTest* _parent;
    };

    void setUp() override {
        setGlobalServiceContext(ServiceContext::make());
        Client::initThread(kClientThreadName);
        _instanceCount.store(0);
    }

    void tearDown() override {
        auto client = Client::releaseCurrent();
        client.reset(nullptr);
    }

    auto getDecoration() noexcept {
        return ClientOutOfLineExecutor::get(Client::getCurrent());
    }

    int countDummies() const noexcept {
        return _instanceCount.load();
    }

    static constexpr auto kClientThreadName = "ClientOutOfLineExecutorTest"_sd;

private:
    friend class DummyInstance;

    AtomicWord<int> _instanceCount;
};

TEST_F(ClientOutOfLineExecutorTest, CheckDecoration) {
    auto decoration = getDecoration();
    ASSERT(decoration);
}

TEST_F(ClientOutOfLineExecutorTest, ScheduleAndExecute) {
    auto thread = stdx::thread([this, handle = getDecoration()->getHandle()]() mutable {
        DummyInstance dummy(this);
        handle.schedule([dummy = std::move(dummy)](const Status& status) {
            ASSERT_OK(status);
            ASSERT_EQ(getThreadName(), ClientOutOfLineExecutorTest::kClientThreadName);
        });
    });
    thread.join();
    ASSERT_EQ(countDummies(), 1);

    getDecoration()->consumeAllTasks();
    ASSERT_EQ(countDummies(), 0);
}

TEST_F(ClientOutOfLineExecutorTest, DestructorExecutesLeftovers) {
    const auto kDummiesCount = 8;
    unittest::Barrier b1(2), b2(2);

    auto thread = stdx::thread([this, kDummiesCount, b1 = &b1, b2 = &b2]() {
        Client::initThread("ThreadWithLeftovers"_sd);

        auto handle = ClientOutOfLineExecutor::get(Client::getCurrent())->getHandle();
        for (auto i = 0; i < kDummiesCount; i++) {
            DummyInstance dummy(this);
            handle.schedule([dummy = std::move(dummy),
                             threadId = stdx::this_thread::get_id()](const Status& status) {
                ASSERT(status == ErrorCodes::ClientDisconnect);
                // Avoid using `getThreadName()` here as it'll cause read-after-delete errors.
                ASSERT_EQ(threadId, stdx::this_thread::get_id());
            });
        }

        b1->countDownAndWait();
        // Wait for the main thread to count dummies.
        b2->countDownAndWait();
    });

    b1.countDownAndWait();
    ASSERT_EQ(countDummies(), kDummiesCount);
    b2.countDownAndWait();

    thread.join();
    ASSERT_EQ(countDummies(), 0);
}

TEST_F(ClientOutOfLineExecutorTest, ScheduleAfterClientThreadReturns) {
    ClientOutOfLineExecutor::QueueHandle handle;

    auto thread = stdx::thread([&handle]() mutable {
        Client::initThread("ClientThread"_sd);
        handle = ClientOutOfLineExecutor::get(Client::getCurrent())->getHandle();
        // Return to destroy the client, and close the task queue.
    });

    thread.join();

    bool taskCalled = false;
    handle.schedule([&taskCalled, threadName = getThreadName()](const Status& status) {
        ASSERT(status == ErrorCodes::CallbackCanceled);
        ASSERT_EQ(getThreadName(), threadName);
        taskCalled = true;
    });
    ASSERT(taskCalled);
}

}  // namespace
}  // namespace mongo
