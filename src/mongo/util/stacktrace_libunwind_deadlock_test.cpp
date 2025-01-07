/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include <chrono>
#include <cstdlib>
#include <functional>
#include <future>
#include <link.h>

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/stacktrace.h"

namespace mongo {
namespace {

/** A thread can wait for a notify issued from another thread. */
class OneShotEvent {
public:
    void notify() {
        return _p.set_value();
    }

    void wait() const {
        return _f.wait();
    }

    bool waitFor(std::chrono::milliseconds dur) const {
        return _f.wait_for(dur) == std::future_status::ready;
    }

private:
    std::promise<void> _p;                  // NOLINT
    std::future<void> _f{_p.get_future()};  // NOLINT
};

using OnPhdrFunc = std::function<int(dl_phdr_info*, size_t)>;

extern "C" int concurrentIterationCallback(dl_phdr_info* info, size_t sz, void* data) {
    return (*static_cast<OnPhdrFunc*>(data))(info, sz);
}

/**
 * Manage an invocation of `dl_iterate_phdr` in a separate thread,
 * with the ability to pause and resume it.
 */
class ConcurrentPhdrIteration {
public:
    /**
     * Launches a thread running a `dl_iterate_phdr`.
     * We arrange for it to pause inside its per-phdr callback.
     */
    ConcurrentPhdrIteration() {
        _thread = unittest::JoinThread{[&] {
            OnPhdrFunc onPhdr{[&](dl_phdr_info*, size_t) {
                _paused.notify();
                _resumed.wait();
                return 1;
            }};
            dl_iterate_phdr(&concurrentIterationCallback, &onPhdr);
        }};
    }

    /** Waits for the `dl_iterate_phdr` to reach its pause point. */
    void wait() {
        return _paused.wait();
    }
    bool waitFor(std::chrono::milliseconds dur) const {
        return _paused.waitFor(dur);
    }

    /** Allows execution to continue beyond the pause point. */
    void resume() {
        _resumed.notify();
    }

private:
    OneShotEvent _paused;
    OneShotEvent _resumed;
    unittest::JoinThread _thread;
};

/**
 * Confirm that `dl_iterate_phdr` runs under a mutex,
 * such that a second run will block waiting for the first.
 */
TEST(DlIteratePhdrDeadlockTest, DeadlockExists) {
    using namespace std::chrono_literals;
    ConcurrentPhdrIteration a;
    a.wait();

    ConcurrentPhdrIteration b;
    ASSERT(!b.waitFor(1s)) << "a blocks b";

    a.resume();
    ASSERT(b.waitFor(1s)) << "b unblocked";
    b.resume();
}

/**
 * Confirms that `getStackTrace` can run without waiting for
 * the completion of a concurrent `dl_iterate_phdr`.
 * Only one `dl_iterate_phdr` can be in progress at a time.
 */
TEST(DlIteratePhdrDeadlockTest, NormalStacktrace) {
    ConcurrentPhdrIteration iter;
    iter.wait();
    getStackTrace();
    iter.resume();
}

/**
 * Confirms that our signal handler stack traces are not blocked
 * by any concurrent `dl_iterate_phdr` operations.
 */
DEATH_TEST(DlIteratePhdrDeadlockTest, SignalHandler, "Aborted") {
    ConcurrentPhdrIteration iter;
    iter.wait();
    std::abort();
}

}  // namespace
}  // namespace mongo
