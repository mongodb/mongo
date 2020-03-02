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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/util/concurrency/thread_pool_test_common.h"

#include <memory>

#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/concurrency/thread_pool_test_fixture.h"

namespace mongo {
namespace {

using ThreadPoolFactory = std::function<std::unique_ptr<ThreadPoolInterface>()>;

class CommonThreadPoolTestFixture : public ThreadPoolTest {
public:
    CommonThreadPoolTestFixture(ThreadPoolFactory makeThreadPool)
        : _makeThreadPool(std::move(makeThreadPool)) {}

private:
    std::unique_ptr<ThreadPoolInterface> makeThreadPool() override {
        return _makeThreadPool();
    }

    ThreadPoolFactory _makeThreadPool;
};

using ThreadPoolTestCaseFactory =
    std::function<std::unique_ptr<::mongo::unittest::Test>(ThreadPoolFactory)>;
using ThreadPoolTestCaseMap = stdx::unordered_map<std::string, ThreadPoolTestCaseFactory>;

static ThreadPoolTestCaseMap& threadPoolTestCaseRegistry() {
    static ThreadPoolTestCaseMap registry;
    return registry;
}

class TptRegistrationAgent {
    TptRegistrationAgent(const TptRegistrationAgent&) = delete;
    TptRegistrationAgent& operator=(const TptRegistrationAgent&) = delete;

public:
    TptRegistrationAgent(const std::string& name, ThreadPoolTestCaseFactory makeTest) {
        auto& entry = threadPoolTestCaseRegistry()[name];
        if (entry) {
            LOGV2_FATAL(34355,
                        "Multiple attempts to register ThreadPoolTest named {name}",
                        "Multiple attempts to register ThreadPoolTest",
                        "name"_attr = name);
        }
        entry = std::move(makeTest);
    }
};

template <typename T>
class TptDeathRegistrationAgent {
    TptDeathRegistrationAgent(const TptDeathRegistrationAgent&) = delete;
    TptDeathRegistrationAgent& operator=(const TptDeathRegistrationAgent&) = delete;

public:
    TptDeathRegistrationAgent(const std::string& name, ThreadPoolTestCaseFactory makeTest) {
        auto& entry = threadPoolTestCaseRegistry()[name];
        if (entry) {
            LOGV2_FATAL(34356,
                        "Multiple attempts to register ThreadPoolDeathTest named {name}",
                        "Multiple attempts to register ThreadPoolDeathTest",
                        "name"_attr = name);
        }
        entry = [makeTest](ThreadPoolFactory makeThreadPool) {
            return std::make_unique<::mongo::unittest::DeathTest<T>>(std::move(makeThreadPool));
        };
    }
};

#define COMMON_THREAD_POOL_TEST(TEST_NAME)                                           \
    class TPT_##TEST_NAME : public CommonThreadPoolTestFixture {                     \
    public:                                                                          \
        TPT_##TEST_NAME(ThreadPoolFactory makeThreadPool)                            \
            : CommonThreadPoolTestFixture(std::move(makeThreadPool)) {}              \
                                                                                     \
    private:                                                                         \
        void _doTest() override;                                                     \
        static inline const TptRegistrationAgent _agent{                             \
            #TEST_NAME, [](ThreadPoolFactory makeThreadPool) {                       \
                return std::make_unique<TPT_##TEST_NAME>(std::move(makeThreadPool)); \
            }};                                                                      \
    };                                                                               \
    void TPT_##TEST_NAME::_doTest()

#define COMMON_THREAD_POOL_DEATH_TEST(TEST_NAME, MATCH_EXPR)                         \
    class TPT_##TEST_NAME : public CommonThreadPoolTestFixture {                     \
    public:                                                                          \
        TPT_##TEST_NAME(ThreadPoolFactory makeThreadPool)                            \
            : CommonThreadPoolTestFixture(std::move(makeThreadPool)) {}              \
        static std::string getPattern() {                                            \
            return MATCH_EXPR;                                                       \
        }                                                                            \
        static bool isRegex() {                                                      \
            return false;                                                            \
        }                                                                            \
        static int getLine() {                                                       \
            return __LINE__;                                                         \
        }                                                                            \
        static std::string getFile() {                                               \
            return __FILE__;                                                         \
        }                                                                            \
                                                                                     \
                                                                                     \
    private:                                                                         \
        void _doTest() override;                                                     \
        static inline const TptDeathRegistrationAgent<TPT_##TEST_NAME> _agent{       \
            #TEST_NAME, [](ThreadPoolFactory makeThreadPool) {                       \
                return std::make_unique<TPT_##TEST_NAME>(std::move(makeThreadPool)); \
            }};                                                                      \
    };                                                                               \
    void TPT_##TEST_NAME::_doTest()

COMMON_THREAD_POOL_TEST(UnusedPool) {
    getThreadPool();
}

COMMON_THREAD_POOL_TEST(CannotScheduleAfterShutdown) {
    auto& pool = getThreadPool();
    pool.shutdown();
    pool.schedule([](auto status) { ASSERT_EQ(status, ErrorCodes::ShutdownInProgress); });
}

COMMON_THREAD_POOL_DEATH_TEST(DieOnDoubleStartUp, "already started") {
    auto& pool = getThreadPool();
    pool.startup();
    pool.startup();
}

namespace {
constexpr auto kExceptionMessage = "No good very bad exception";
}

COMMON_THREAD_POOL_DEATH_TEST(DieWhenExceptionBubblesUp, kExceptionMessage) {
    auto& pool = getThreadPool();
    pool.startup();
    pool.schedule([](auto status) {
        uassertStatusOK(Status({ErrorCodes::BadValue, kExceptionMessage}));
    });
    pool.shutdown();
    pool.join();
}

COMMON_THREAD_POOL_DEATH_TEST(DieOnDoubleJoin, "Attempted to join pool") {
    auto& pool = getThreadPool();
    pool.shutdown();
    pool.join();
    pool.join();
}

COMMON_THREAD_POOL_TEST(PoolDestructorExecutesRemainingTasks) {
    auto& pool = getThreadPool();
    bool executed = false;
    pool.schedule([&executed](auto status) {
        ASSERT_OK(status);
        executed = true;
    });
    deleteThreadPool();
    ASSERT_EQ(executed, true);
}

COMMON_THREAD_POOL_TEST(PoolJoinExecutesRemainingTasks) {
    auto& pool = getThreadPool();
    bool executed = false;
    pool.schedule([&executed](auto status) {
        ASSERT_OK(status);
        executed = true;
    });
    pool.shutdown();
    pool.join();
    ASSERT_EQ(executed, true);
}

COMMON_THREAD_POOL_TEST(RepeatedScheduleDoesntSmashStack) {
    const std::size_t depth = 10000ul;
    auto& pool = getThreadPool();
    std::function<void()> func;
    std::size_t n = 0;
    auto mutex = MONGO_MAKE_LATCH();
    stdx::condition_variable condvar;
    func = [&pool, &n, &func, &condvar, &mutex, depth]() {
        stdx::unique_lock<Latch> lk(mutex);
        if (n < depth) {
            n++;
            lk.unlock();
            pool.schedule([&](auto status) {
                ASSERT_OK(status);
                func();
            });
        } else {
            pool.shutdown();
            condvar.notify_one();
        }
    };
    func();
    pool.startup();
    pool.join();

    stdx::unique_lock<Latch> lk(mutex);
    condvar.wait(lk, [&n, depth] { return n == depth; });
}

}  // namespace

void addTestsForThreadPool(const std::string& suiteName, ThreadPoolFactory makeThreadPool) {
    auto& suite = unittest::Suite::getSuite(suiteName);
    for (auto testCase : threadPoolTestCaseRegistry()) {
        suite.add(str::stream() << suiteName << "::" << testCase.first,
                  __FILE__,
                  [testCase, makeThreadPool] { testCase.second(makeThreadPool)->run(); });
    }
}

}  // namespace mongo
