/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/util/concurrency/thread_pool_test_common.h"

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/concurrency/thread_pool_test_fixture.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using ThreadPoolFactory = stdx::function<std::unique_ptr<ThreadPoolInterface>()>;

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
    stdx::function<std::unique_ptr<::mongo::unittest::Test>(ThreadPoolFactory)>;
using ThreadPoolTestCaseMap = unordered_map<std::string, ThreadPoolTestCaseFactory>;

static ThreadPoolTestCaseMap& threadPoolTestCaseRegistry() {
    static ThreadPoolTestCaseMap registry;
    return registry;
}

class TptRegistrationAgent {
    MONGO_DISALLOW_COPYING(TptRegistrationAgent);

public:
    TptRegistrationAgent(const std::string& name, ThreadPoolTestCaseFactory makeTest) {
        auto& entry = threadPoolTestCaseRegistry()[name];
        if (entry) {
            severe() << "Multiple attempts to register ThreadPoolTest named " << name;
            fassertFailed(34355);
        }
        entry = std::move(makeTest);
    }
};

template <typename T>
class TptDeathRegistrationAgent {
    MONGO_DISALLOW_COPYING(TptDeathRegistrationAgent);

public:
    TptDeathRegistrationAgent(const std::string& name, ThreadPoolTestCaseFactory makeTest) {
        auto& entry = threadPoolTestCaseRegistry()[name];
        if (entry) {
            severe() << "Multiple attempts to register ThreadPoolDeathTest named " << name;
            fassertFailed(34356);
        }
        entry = [makeTest](ThreadPoolFactory makeThreadPool) {
            return stdx::make_unique<::mongo::unittest::DeathTest<T>>(std::move(makeThreadPool));
        };
    }
};

#define COMMON_THREAD_POOL_TEST(TEST_NAME)                                        \
    class TPT_##TEST_NAME : public CommonThreadPoolTestFixture {                  \
    public:                                                                       \
        TPT_##TEST_NAME(ThreadPoolFactory makeThreadPool)                         \
            : CommonThreadPoolTestFixture(std::move(makeThreadPool)) {}           \
                                                                                  \
    private:                                                                      \
        void _doTest() override;                                                  \
        static const TptRegistrationAgent _agent;                                 \
    };                                                                            \
    const TptRegistrationAgent TPT_##TEST_NAME::_agent(                           \
        #TEST_NAME, [](ThreadPoolFactory makeThreadPool) {                        \
            return stdx::make_unique<TPT_##TEST_NAME>(std::move(makeThreadPool)); \
        });                                                                       \
    void TPT_##TEST_NAME::_doTest()

#define COMMON_THREAD_POOL_DEATH_TEST(TEST_NAME, MATCH_EXPR)                      \
    class TPT_##TEST_NAME : public CommonThreadPoolTestFixture {                  \
    public:                                                                       \
        TPT_##TEST_NAME(ThreadPoolFactory makeThreadPool)                         \
            : CommonThreadPoolTestFixture(std::move(makeThreadPool)) {}           \
                                                                                  \
    private:                                                                      \
        void _doTest() override;                                                  \
        static const TptDeathRegistrationAgent<TPT_##TEST_NAME> _agent;           \
    };                                                                            \
    const TptDeathRegistrationAgent<TPT_##TEST_NAME> TPT_##TEST_NAME::_agent(     \
        #TEST_NAME, [](ThreadPoolFactory makeThreadPool) {                        \
            return stdx::make_unique<TPT_##TEST_NAME>(std::move(makeThreadPool)); \
        });                                                                       \
    std::string getDeathTestPattern(TPT_##TEST_NAME*) {                           \
        return MATCH_EXPR;                                                        \
    }                                                                             \
    void TPT_##TEST_NAME::_doTest()

COMMON_THREAD_POOL_TEST(UnusedPool) {
    getThreadPool();
}

COMMON_THREAD_POOL_TEST(CannotScheduleAfterShutdown) {
    auto& pool = getThreadPool();
    pool.shutdown();
    ASSERT_EQ(ErrorCodes::ShutdownInProgress, pool.schedule([] {}));
}

COMMON_THREAD_POOL_DEATH_TEST(DieOnDoubleStartUp, "it has already started") {
    auto& pool = getThreadPool();
    pool.startup();
    pool.startup();
}

COMMON_THREAD_POOL_DEATH_TEST(DieWhenExceptionBubblesUp, "Exception escaped task in") {
    auto& pool = getThreadPool();
    pool.startup();
    ASSERT_OK(pool.schedule([] {
        uassertStatusOK(Status({ErrorCodes::BadValue, "No good very bad exception"}));
    }));
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
    ASSERT_OK(pool.schedule([&executed] { executed = true; }));
    deleteThreadPool();
    ASSERT_EQ(executed, true);
}

COMMON_THREAD_POOL_TEST(PoolJoinExecutesRemainingTasks) {
    auto& pool = getThreadPool();
    bool executed = false;
    ASSERT_OK(pool.schedule([&executed] { executed = true; }));
    pool.shutdown();
    pool.join();
    ASSERT_EQ(executed, true);
}

COMMON_THREAD_POOL_TEST(RepeatedScheduleDoesntSmashStack) {
    const std::size_t depth = 10000ul;
    auto& pool = getThreadPool();
    stdx::function<void()> func;
    std::size_t n = 0;
    stdx::mutex mutex;
    stdx::condition_variable condvar;
    func = [&pool, &n, &func, &condvar, &mutex, depth] {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        if (n < depth) {
            n++;
            lk.unlock();
            ASSERT_OK(pool.schedule(func));
        } else {
            pool.shutdown();
            condvar.notify_one();
        }
    };
    func();
    pool.startup();
    pool.join();

    stdx::unique_lock<stdx::mutex> lk(mutex);
    condvar.wait(lk, [&n, depth] { return n == depth; });
}

}  // namespace

void addTestsForThreadPool(const std::string& suiteName, ThreadPoolFactory makeThreadPool) {
    auto suite = unittest::Suite::getSuite(suiteName);
    for (auto testCase : threadPoolTestCaseRegistry()) {
        suite->add(str::stream() << suiteName << "::" << testCase.first,
                   [testCase, makeThreadPool] { testCase.second(makeThreadPool)->run(); });
    }
}

}  // namespace mongo
