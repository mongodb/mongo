/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kExecutor;

#include "mongo/platform/basic.h"

#include "mongo/transport/service_executor_reserved.h"

#include "mongo/db/server_parameters.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_entry_point_utils.h"
#include "mongo/transport/service_executor_task_names.h"
#include "mongo/util/log.h"
#include "mongo/util/net/thread_idle_callback.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace transport {
namespace {

// Tasks scheduled with MayRecurse may be called recursively if the recursion depth is below this
// value.
MONGO_EXPORT_SERVER_PARAMETER(reservedServiceExecutorRecursionLimit, int, 8);

constexpr auto kThreadsRunning = "threadsRunning"_sd;
constexpr auto kExecutorLabel = "executor"_sd;
constexpr auto kExecutorName = "reserved"_sd;
constexpr auto kReadyThreads = "readyThreads"_sd;
constexpr auto kStartingThreads = "startingThreads"_sd;
}  // namespace

thread_local std::deque<ServiceExecutor::Task> ServiceExecutorReserved::_localWorkQueue = {};
thread_local int ServiceExecutorReserved::_localRecursionDepth = 0;
thread_local int64_t ServiceExecutorReserved::_localThreadIdleCounter = 0;

ServiceExecutorReserved::ServiceExecutorReserved(ServiceContext* ctx,
                                                 std::string name,
                                                 size_t reservedThreads)
    : _name(std::move(name)), _reservedThreads(reservedThreads) {}

Status ServiceExecutorReserved::start() {
    {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _stillRunning.store(true);
        _numStartingThreads = _reservedThreads;
    }

    for (size_t i = 0; i < _reservedThreads; i++) {
        auto status = _startWorker();
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status ServiceExecutorReserved::_startWorker() {
    log() << "Starting new worker thread for " << _name << " service executor";
    return launchServiceWorkerThread([this] {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _numRunningWorkerThreads.addAndFetch(1);
        auto numRunningGuard = MakeGuard([&] {
            _numRunningWorkerThreads.subtractAndFetch(1);
            _shutdownCondition.notify_one();
        });

        _numStartingThreads--;
        _numReadyThreads++;

        while (_stillRunning.load()) {
            _threadWakeup.wait(lk, [&] { return (!_stillRunning.load() || !_readyTasks.empty()); });

            if (!_stillRunning.loadRelaxed()) {
                break;
            }

            if (_readyTasks.empty()) {
                continue;
            }

            auto task = std::move(_readyTasks.front());
            _readyTasks.pop_front();
            _numReadyThreads -= 1;
            bool launchReplacement = false;
            if (_numReadyThreads + _numStartingThreads < _reservedThreads) {
                _numStartingThreads++;
                launchReplacement = true;
            }

            lk.unlock();

            if (launchReplacement) {
                auto threadStartStatus = _startWorker();
                if (!threadStartStatus.isOK()) {
                    warning() << "Could not start new reserve worker thread: " << threadStartStatus;
                }
            }

            _localWorkQueue.emplace_back(std::move(task));
            while (!_localWorkQueue.empty() && _stillRunning.loadRelaxed()) {
                _localRecursionDepth = 1;
                _localWorkQueue.front()();
                _localWorkQueue.pop_front();
            }

            lk.lock();
            if (_numReadyThreads + 1 > _reservedThreads) {
                break;
            } else {
                _numReadyThreads += 1;
            }
        }

        LOG(3) << "Exiting worker thread in " << _name << " service executor";
    });
}


Status ServiceExecutorReserved::shutdown(Milliseconds timeout) {
    LOG(3) << "Shutting down reserved executor";

    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _stillRunning.store(false);
    _threadWakeup.notify_all();

    bool result = _shutdownCondition.wait_for(lock, timeout.toSystemDuration(), [this]() {
        return _numRunningWorkerThreads.load() == 0;
    });

    return result
        ? Status::OK()
        : Status(ErrorCodes::Error::ExceededTimeLimit,
                 "reserved executor couldn't shutdown all worker threads within time limit.");
}

Status ServiceExecutorReserved::schedule(Task task,
                                         ScheduleFlags flags,
                                         ServiceExecutorTaskName taskName) {
    if (!_stillRunning.load()) {
        return Status{ErrorCodes::ShutdownInProgress, "Executor is not running"};
    }

    if (!_localWorkQueue.empty()) {
        /*
         * In perf testing we found that yielding after running a each request produced
         * at 5% performance boost in microbenchmarks if the number of worker threads
         * was greater than the number of available cores.
         */
        if (flags & ScheduleFlags::kMayYieldBeforeSchedule) {
            if ((_localThreadIdleCounter++ & 0xf) == 0) {
                markThreadIdle();
            }
        }

        // Execute task directly (recurse) if allowed by the caller as it produced better
        // performance in testing. Try to limit the amount of recursion so we don't blow up the
        // stack, even though this shouldn't happen with this executor that uses blocking network
        // I/O.
        if ((flags & ScheduleFlags::kMayRecurse) &&
            (_localRecursionDepth < reservedServiceExecutorRecursionLimit.loadRelaxed())) {
            ++_localRecursionDepth;
            task();
        } else {
            _localWorkQueue.emplace_back(std::move(task));
        }
        return Status::OK();
    }

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _readyTasks.push_back(std::move(task));
    _threadWakeup.notify_one();

    return Status::OK();
}

void ServiceExecutorReserved::appendStats(BSONObjBuilder* bob) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    *bob << kExecutorLabel << kExecutorName << kThreadsRunning
         << static_cast<int>(_numRunningWorkerThreads.loadRelaxed()) << kReadyThreads
         << static_cast<int>(_numReadyThreads) << kStartingThreads
         << static_cast<int>(_numStartingThreads);
}

}  // namespace transport
}  // namespace mongo
