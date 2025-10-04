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


#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/quick_exit.h"

#include <mutex>
#include <stack>
#include <thread>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

namespace {

stdx::mutex shutdownMutex;
stdx::condition_variable shutdownTasksComplete;
boost::optional<ExitCode> shutdownExitCode;
bool shutdownTasksInProgress = false;
AtomicWord<unsigned> shutdownFlag;
std::stack<ShutdownTask> shutdownTasks;
stdx::thread::id shutdownTasksThreadId;

void runRegisteredShutdownTasks(decltype(shutdownTasks) tasks,
                                const ShutdownTaskArgs& shutdownArgs) noexcept {
    while (!tasks.empty()) {
        const auto& task = tasks.top();
        task(shutdownArgs);
        tasks.pop();
    }
}

// The logAndQuickExit_inlock() function should be called while holding the 'shutdownMutex' to
// prevent multiple threads from attempting to log that they are exiting. The quickExit() function
// has its own 'quickExitMutex' to prohibit multiple threads from attempting to call _exit().
MONGO_COMPILER_NORETURN void logAndQuickExit_inlock() {
    ExitCode code = shutdownExitCode.value();
    LOGV2(23138, "Shutting down", "exitCode"_attr = code);
    quickExit(code);
}

void setShutdownFlag() {
    shutdownFlag.fetchAndAdd(1);
}

}  // namespace

bool globalInShutdownDeprecated() {
    return shutdownFlag.loadRelaxed() != 0;
}

ExitCode waitForShutdown() {
    stdx::unique_lock<stdx::mutex> lk(shutdownMutex);
    shutdownTasksComplete.wait(lk, [] {
        const auto shutdownStarted = static_cast<bool>(shutdownExitCode);
        return shutdownStarted && !shutdownTasksInProgress;
    });

    return shutdownExitCode.value();
}

void registerShutdownTask(ShutdownTask task) {
    stdx::lock_guard<stdx::mutex> lock(shutdownMutex);
    invariant(!globalInShutdownDeprecated());
    shutdownTasks.emplace(std::move(task));
}

void shutdown(ExitCode code, const ShutdownTaskArgs& shutdownArgs) {
    decltype(shutdownTasks) localTasks;

    {
        stdx::unique_lock<stdx::mutex> lock(shutdownMutex);

        if (shutdownTasksInProgress) {
            // Someone better have called shutdown in some form already.
            invariant(globalInShutdownDeprecated());

            // Re-entrant calls to shutdown are not allowed.
            invariant(shutdownTasksThreadId != stdx::this_thread::get_id());

            ExitCode originallyRequestedCode = shutdownExitCode.value();
            if (code != originallyRequestedCode) {
                LOGV2(23139,
                      "Conflicting exit code at shutdown",
                      "originalExitCode"_attr = originallyRequestedCode,
                      "newExitCode"_attr = code);
            }

            // Wait for the shutdown tasks to complete
            while (shutdownTasksInProgress)
                shutdownTasksComplete.wait(lock);

            logAndQuickExit_inlock();
        }

        setShutdownFlag();
        shutdownExitCode.emplace(code);
        shutdownTasksInProgress = true;
        shutdownTasksThreadId = stdx::this_thread::get_id();

        localTasks.swap(shutdownTasks);
    }

    runRegisteredShutdownTasks(std::move(localTasks), shutdownArgs);

    {
        stdx::lock_guard<stdx::mutex> lock(shutdownMutex);
        shutdownTasksInProgress = false;

        shutdownTasksComplete.notify_all();

        logAndQuickExit_inlock();
    }
}

void shutdownNoTerminate(const ShutdownTaskArgs& shutdownArgs) {
    decltype(shutdownTasks) localTasks;

    {
        stdx::lock_guard<stdx::mutex> lock(shutdownMutex);

        if (globalInShutdownDeprecated())
            return;

        setShutdownFlag();
        shutdownTasksInProgress = true;
        shutdownTasksThreadId = stdx::this_thread::get_id();

        localTasks.swap(shutdownTasks);
    }

    runRegisteredShutdownTasks(std::move(localTasks), shutdownArgs);

    {
        stdx::lock_guard<stdx::mutex> lock(shutdownMutex);
        shutdownTasksInProgress = false;
        shutdownExitCode.emplace(ExitCode::clean);
    }

    shutdownTasksComplete.notify_all();
}
}  // namespace mongo
