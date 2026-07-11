// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/client_out_of_line_executor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"

#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

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
        Client::initThread(kClientThreadName, getGlobalServiceContext()->getService());
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

    static constexpr auto kClientThreadName = "ClientOutOfLineExecutorTest"sv;

private:
    friend class DummyInstance;

    Atomic<int> _instanceCount;
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
        Client::initThread("ThreadWithLeftovers"sv, getGlobalServiceContext()->getService());

        auto handle = ClientOutOfLineExecutor::get(Client::getCurrent())->getHandle();
        for (auto i = 0; i < kDummiesCount; i++) {
            DummyInstance dummy(this);
            handle.schedule([dummy = std::move(dummy),
                             threadId = std::this_thread::get_id()](const Status& status) {
                ASSERT(status == ErrorCodes::ClientDisconnect);
                // Avoid using `getThreadName()` here as it'll cause read-after-delete errors.
                ASSERT_EQ(threadId, std::this_thread::get_id());
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
        Client::initThread("ClientThread"sv, getGlobalServiceContext()->getService());
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

TEST_F(ClientOutOfLineExecutorTest, SkipShutdownWhenNoTaskIsScheduled) {
    ClientOutOfLineExecutor executor;
}

using ClientOutOfLineExecutorTestDeathTest = ClientOutOfLineExecutorTest;
DEATH_TEST_F(ClientOutOfLineExecutorTestDeathTest,
             RequireShutdownAfterAcquiringHandles,
             "invariant") {
    ClientOutOfLineExecutor executor;
    executor.getHandle();
}

DEATH_TEST_F(ClientOutOfLineExecutorTestDeathTest,
             RequireShutdownAfterSchedulingTasks,
             "invariant") {
    ClientOutOfLineExecutor executor;
    executor.schedule([](Status) {});
}

}  // namespace
}  // namespace mongo
