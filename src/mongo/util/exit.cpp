/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl;

#include "mongo/platform/basic.h"

#include "mongo/util/exit.h"

#include <boost/optional.hpp>
#include <stack>

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/log.h"
#include "mongo/util/quick_exit.h"

namespace mongo {

namespace {

stdx::mutex shutdownMutex;
stdx::condition_variable shutdownTasksComplete;
boost::optional<ExitCode> shutdownExitCode;
bool shutdownTasksInProgress = false;
AtomicUInt32 shutdownFlag;
std::stack<stdx::function<void()>> shutdownTasks;
stdx::thread::id shutdownTasksThreadId;

void runTasks(decltype(shutdownTasks) tasks) {
    while (!tasks.empty()) {
        const auto& task = tasks.top();
        try {
            task();
        } catch (...) {
            std::terminate();
        }
        tasks.pop();
    }
}

MONGO_COMPILER_NORETURN void logAndQuickExit(ExitCode code) {
    log() << "shutting down with code:" << code;
    quickExit(code);
}

void setShutdownFlag() {
    shutdownFlag.fetchAndAdd(1);
}

}  // namespace

bool inShutdown() {
    return shutdownFlag.loadRelaxed() != 0;
}

bool inShutdownStrict() {
    return shutdownFlag.load() != 0;
}

ExitCode waitForShutdown() {
    stdx::unique_lock<stdx::mutex> lk(shutdownMutex);
    shutdownTasksComplete.wait(lk, [] { return static_cast<bool>(shutdownExitCode); });

    return shutdownExitCode.get();
}

void registerShutdownTask(stdx::function<void()> task) {
    stdx::lock_guard<stdx::mutex> lock(shutdownMutex);
    invariant(!inShutdown());
    shutdownTasks.emplace(std::move(task));
}

void shutdown(ExitCode code) {
    decltype(shutdownTasks) localTasks;

    {
        stdx::unique_lock<stdx::mutex> lock(shutdownMutex);

        if (shutdownTasksInProgress) {
            // Someone better have called shutdown in some form already.
            invariant(inShutdown());

            // Re-entrant calls to shutdown are not allowed.
            invariant(shutdownTasksThreadId != stdx::this_thread::get_id());

            // Wait for the shutdown tasks to complete
            while (shutdownTasksInProgress)
                shutdownTasksComplete.wait(lock);

            logAndQuickExit(code);
        }

        setShutdownFlag();
        shutdownTasksInProgress = true;
        shutdownTasksThreadId = stdx::this_thread::get_id();

        localTasks.swap(shutdownTasks);
    }

    runTasks(std::move(localTasks));

    {
        stdx::lock_guard<stdx::mutex> lock(shutdownMutex);
        shutdownTasksInProgress = false;
        shutdownExitCode.emplace(code);
    }

    shutdownTasksComplete.notify_all();

    logAndQuickExit(code);
}

void shutdownNoTerminate() {
    decltype(shutdownTasks) localTasks;

    {
        stdx::lock_guard<stdx::mutex> lock(shutdownMutex);

        if (inShutdown())
            return;

        setShutdownFlag();
        shutdownTasksInProgress = true;
        shutdownTasksThreadId = stdx::this_thread::get_id();

        localTasks.swap(shutdownTasks);
    }

    runTasks(std::move(localTasks));

    {
        stdx::lock_guard<stdx::mutex> lock(shutdownMutex);
        shutdownTasksInProgress = false;
    }

    shutdownTasksComplete.notify_all();
}

}  // namespace mongo
