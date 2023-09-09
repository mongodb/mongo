


#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kExecutor;

#include "mongo/platform/basic.h"

#include "mongo/transport/service_executor_reserved.h"

#include "mongo/db/server_parameters.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_entry_point_utils.h"
#include "mongo/transport/service_executor_task_names.h"
#include "mongo/transport/thread_idle_callback.h"
#include "mongo/util/log.h"
#include "mongo/util/processinfo.h"

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
                                                 const std::string& name,
                                                 size_t reservedThreads)
    : _name(std::move(name)), _reservedThreads(reservedThreads), _threadGroups(reservedThreads) {}

Status ServiceExecutorReserved::start() {
    {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _stillRunning.store(true, std::memory_order_relaxed);
        _numStartingThreads = _reservedThreads;
    }

    for (size_t i = 0; i < _reservedThreads; i++) {
        auto status = _startWorker(i);
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status ServiceExecutorReserved::_startWorker(uint16_t groupId) {
    log() << "Starting new worker thread for " << _name << " service executor";
    return launchServiceWorkerThread([this, threadGroupId = groupId] {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _numRunningWorkerThreads.addAndFetch(1);
        auto numRunningGuard = MakeGuard([&] {
            _numRunningWorkerThreads.subtractAndFetch(1);
            _shutdownCondition.notify_one();
        });

        _numStartingThreads--;
        _numReadyThreads++;

        while (_stillRunning.load()) {


            if (!_stillRunning.load(std::memory_order_relaxed)) {
                break;
            }

            ThreadGroup& threadGroup = _threadGroups[threadGroupId];
            threadGroup.TrySleep();

            size_t cnt = 0;
            if (threadGroup.resume_queue_size_.load(std::memory_order_relaxed) > 0) {
                std::array<Task, 100> task_bulk;
                cnt =
                    threadGroup.resume_queue_.try_dequeue_bulk(task_bulk.begin(), task_bulk.size());

                for (size_t idx = 0; idx < cnt; ++idx) {
                    _localWorkQueue.emplace_back(std::move(task_bulk[idx]));
                }
            }

            if (cnt == 0 && threadGroup.task_queue_size_.load(std::memory_order_relaxed) > 0) {
                std::array<Task, 100> task_bulk;
                cnt = threadGroup.task_queue_.try_dequeue_bulk(task_bulk.begin(), task_bulk.size());

                for (size_t idx = 0; idx < cnt; ++idx) {
                    _localWorkQueue.emplace_back(std::move(task_bulk[idx]));
                }
            }

            if (cnt == 0) {
                continue;
            }

            _numReadyThreads -= 1;

            bool launchReplacement = false;
            if (_numReadyThreads + _numStartingThreads < _reservedThreads) {
                _numStartingThreads++;
                launchReplacement = true;
            }

            lk.unlock();

            if (launchReplacement) {
                auto threadStartStatus = _startWorker(threadGroupId);
                if (!threadStartStatus.isOK()) {
                    warning() << "Could not start new reserve worker thread: " << threadStartStatus;
                }
            }

            while (!_localWorkQueue.empty() && _stillRunning.load(std::memory_order_relaxed)) {
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
    _stillRunning.store(false, std::memory_order_relaxed);
    _threadWakeup.notify_all();

    for (ThreadGroup& thd_group : _threadGroups) {
        thd_group.Terminate();
    }

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

Status ServiceExecutorReserved::schedule(Task task,
                                         ScheduleFlags flags,
                                         ServiceExecutorTaskName taskName,
                                         uint16_t thd_group_id) {
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

    _threadGroups[thd_group_id].EnqueueTask(std::move(task));

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
