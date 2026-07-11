// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/condition_variable.h"
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

std::mutex shutdownMutex;
stdx::condition_variable shutdownTasksComplete;
boost::optional<ExitCode> shutdownExitCode;
bool shutdownTasksInProgress = false;
Atomic<unsigned> shutdownFlag;
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
    std::unique_lock<std::mutex> lk(shutdownMutex);
    shutdownTasksComplete.wait(lk, [] {
        const auto shutdownStarted = static_cast<bool>(shutdownExitCode);
        return shutdownStarted && !shutdownTasksInProgress;
    });

    return shutdownExitCode.value();
}

void registerShutdownTask(ShutdownTask task) {
    std::lock_guard<std::mutex> lock(shutdownMutex);
    invariant(!globalInShutdownDeprecated());
    shutdownTasks.emplace(std::move(task));
}

void shutdown(ExitCode code, const ShutdownTaskArgs& shutdownArgs) {
    decltype(shutdownTasks) localTasks;

    {
        std::unique_lock<std::mutex> lock(shutdownMutex);

        if (shutdownTasksInProgress) {
            // Someone better have called shutdown in some form already.
            invariant(globalInShutdownDeprecated());

            // Re-entrant calls to shutdown are not allowed.
            invariant(shutdownTasksThreadId != std::this_thread::get_id());

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
        shutdownTasksThreadId = std::this_thread::get_id();

        localTasks.swap(shutdownTasks);
    }

    runRegisteredShutdownTasks(std::move(localTasks), shutdownArgs);

    {
        std::lock_guard<std::mutex> lock(shutdownMutex);
        shutdownTasksInProgress = false;

        shutdownTasksComplete.notify_all();

        logAndQuickExit_inlock();
    }
}

void shutdownNoTerminate(const ShutdownTaskArgs& shutdownArgs) {
    decltype(shutdownTasks) localTasks;

    {
        std::lock_guard<std::mutex> lock(shutdownMutex);

        if (globalInShutdownDeprecated())
            return;

        setShutdownFlag();
        shutdownTasksInProgress = true;
        shutdownTasksThreadId = std::this_thread::get_id();

        localTasks.swap(shutdownTasks);
    }

    runRegisteredShutdownTasks(std::move(localTasks), shutdownArgs);

    {
        std::lock_guard<std::mutex> lock(shutdownMutex);
        shutdownTasksInProgress = false;
        shutdownExitCode.emplace(ExitCode::clean);
    }

    shutdownTasksComplete.notify_all();
}
}  // namespace mongo
