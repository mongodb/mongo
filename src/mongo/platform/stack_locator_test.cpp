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

#include "mongo/platform/basic.h"

#include "mongo/platform/stack_locator.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(StackLocator, StacLocatorFindsStackOfTestExecutorThread) {
    const StackLocator locator;

    const auto begin = locator.begin();
    ASSERT_TRUE(nullptr != begin);

    const auto end = locator.end();
    ASSERT_TRUE(nullptr != end);

    ASSERT_TRUE(begin != end);

    const auto available = locator.available();
    ASSERT_TRUE(available);
    ASSERT_TRUE(available.get() > 0);

    const auto size = locator.size();
    ASSERT_TRUE(size);
    ASSERT_TRUE(size.get() > 0);
    ASSERT_TRUE(size.get() > available.get());
}

TEST(StackLocator, StacksGrowsDown) {
    // The current implementation assumes a downward growing stack. Write a test
    // that confirms that the current platform is downward growing, so that if the
    // system is ever ported to an upward growing stack, we get a test failure.

    const StackLocator locator;
    ASSERT_TRUE(nullptr != locator.begin());
    ASSERT_TRUE(nullptr != locator.end());

    // NOTE: Technically, comparing pointers for ordering is UB if
    // they aren't in the same aggregate, but we are already out
    // with the dragons at the edge of the map.
    ASSERT_TRUE(locator.begin() > locator.end());
}

TEST(StackLocator, StackLocatorFindsStackOfStdThread) {
    bool foundBounds = false;

    stdx::thread thr([&] {
        const StackLocator locator;
        auto avail = locator.available();
        foundBounds = static_cast<bool>(avail);
    });
    thr.join();
    ASSERT_TRUE(foundBounds);
}

struct LocatorThreadHelper {
#ifdef _WIN32
    static DWORD WINAPI run(LPVOID arg) {
        static_cast<LocatorThreadHelper*>(arg)->_run();
        return 0;
    }
#else
    static void* run(void* arg) {
        static_cast<LocatorThreadHelper*>(arg)->_run();
        return nullptr;
    }
#endif

    void _run() {
        const StackLocator locator;
        located = static_cast<bool>(locator.available());
        if (located)
            size = locator.size().get();
    }

    bool located = false;
    size_t size = 0;
};

TEST(StackLocator, StackLocatorFindsStackOfNativeThreadWithDefaultStack) {
    LocatorThreadHelper helper;

#ifdef _WIN32

    HANDLE thread = CreateThread(nullptr, 0, &LocatorThreadHelper::run, &helper, 0, nullptr);
    ASSERT_NE(WAIT_FAILED, WaitForSingleObject(thread, INFINITE));

#else

    pthread_attr_t attrs;
    ASSERT_EQ(0, pthread_attr_init(&attrs));
    pthread_t thread;
    ASSERT_EQ(0, pthread_create(&thread, &attrs, &LocatorThreadHelper::run, &helper));
    ASSERT_EQ(0, pthread_join(thread, nullptr));

#endif

    ASSERT_TRUE(helper.located);
}

TEST(StackLocator, StackLocatorFindStackOfNativeThreadWithCustomStack) {
    const size_t kThreadStackSize = 64 * 1024 * 1024;

#ifdef _WIN32

    LocatorThreadHelper helperNoCommit;
    HANDLE thread = CreateThread(nullptr,
                                 kThreadStackSize,
                                 &LocatorThreadHelper::run,
                                 &helperNoCommit,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION,
                                 nullptr);
    ASSERT_NE(WAIT_FAILED, WaitForSingleObject(thread, INFINITE));
    ASSERT_TRUE(helperNoCommit.located);
    ASSERT_EQ(kThreadStackSize, helperNoCommit.size);

    LocatorThreadHelper helperCommit;
    thread = CreateThread(
        nullptr, kThreadStackSize, &LocatorThreadHelper::run, &helperCommit, 0, nullptr);
    ASSERT_NE(WAIT_FAILED, WaitForSingleObject(thread, INFINITE));
    ASSERT_TRUE(helperCommit.located);
    ASSERT_TRUE(kThreadStackSize <= helperCommit.size);

#else

    LocatorThreadHelper helper;
    pthread_attr_t attrs;
    ASSERT_EQ(0, pthread_attr_init(&attrs));
    ASSERT_EQ(0, pthread_attr_setstacksize(&attrs, kThreadStackSize));
    pthread_t thread;
    ASSERT_EQ(0, pthread_create(&thread, &attrs, &LocatorThreadHelper::run, &helper));
    ASSERT_EQ(0, pthread_join(thread, nullptr));
    ASSERT_TRUE(helper.located);
    ASSERT_TRUE(kThreadStackSize <= helper.size);

#endif
}

}  // namespace
}  // namespace mongo
