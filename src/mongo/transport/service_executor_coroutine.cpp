#include "mongo/base/string_data.h"
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kExecutor;

#include "mongo/db/server_parameters.h"
#include "mongo/transport/service_entry_point_utils.h"
#include "mongo/transport/service_executor_coroutine.h"
#include "mongo/transport/service_executor_task_names.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/log.h"

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


void ThreadGroup::EnqueueTask(Task task) {
    task_queue_size_.fetch_add(1, std::memory_order_relaxed);
    task_queue_.enqueue(std::move(task));

    NotifyIfAsleep();
}

void ThreadGroup::ResumeTask(Task task) {
    resume_queue_size_.fetch_add(1, std::memory_order_relaxed);
    resume_queue_.enqueue(std::move(task));

    NotifyIfAsleep();
}

void ThreadGroup::NotifyIfAsleep() {
    if (_is_sleep.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lk(_sleep_mux);
        _sleep_cv.notify_one();
    }
}


void ThreadGroup::TrySleep() {

    // If there are tasks in the , does not sleep.
    if (!IsIdle()) {
        return;
    }

    // Sets the sleep flag before entering the critical section. std::memory_order_relaxed is
    // good enough, because the following mutex ensures that this instruction happens before the
    // critical section
    _is_sleep.store(true, std::memory_order_relaxed);

    std::unique_lock<std::mutex> lk(_sleep_mux);

    // Double checkes again in the critical section before going to sleep. If additional tasks
    // are enqueued, does not sleep.
    if (!IsIdle()) {
        _is_sleep.store(false, std::memory_order_relaxed);
        return;
    }

    _sleep_cv.wait(lk, [this] { return !IsIdle(); });

    // Woken up from sleep.
    _is_sleep.store(false, std::memory_order_relaxed);
}

void ThreadGroup::Terminate() {
    _is_terminated.store(true, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lk(_sleep_mux);
    _sleep_cv.notify_one();
}


thread_local std::deque<ServiceExecutor::Task> ServiceExecutorCoroutine::_localWorkQueue = {};
thread_local int ServiceExecutorCoroutine::_localRecursionDepth = 0;
thread_local int64_t ServiceExecutorCoroutine::_localThreadIdleCounter = 0;

ServiceExecutorCoroutine::ServiceExecutorCoroutine(ServiceContext* ctx, size_t reservedThreads)
    : _name{"coroutine"}, _reservedThreads(reservedThreads), _threadGroups(reservedThreads) {}

Status ServiceExecutorCoroutine::start() {
    MONGO_LOG(0) << "ServiceExecutorCoroutine::start";
    {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _stillRunning.store(true, std::memory_order_relaxed);
    }

    for (size_t i = 0; i < _reservedThreads; i++) {
        auto status = _startWorker(i);
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status ServiceExecutorCoroutine::_startWorker(uint16_t groupId) {
    MONGO_LOG(0) << "Starting new worker thread for " << _name << " service executor. "
                 << " group id: " << groupId;

    return launchServiceWorkerThread([this, threadGroupId = groupId] {
        std::string threadName("thread_group_" + std::to_string(threadGroupId));
        StringData threadNameSD(threadName);

        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _numRunningWorkerThreads.addAndFetch(1);
        auto numRunningGuard = MakeGuard([&] {
            _numRunningWorkerThreads.subtractAndFetch(1);
            _shutdownCondition.notify_one();
        });
        lk.unlock();

        ThreadGroup& threadGroup = _threadGroups[threadGroupId];
        while (_stillRunning.load()) {

            if (!_stillRunning.load(std::memory_order_relaxed)) {
                break;
            }

            threadGroup.TrySleep();

            size_t cnt = 0;
            if (threadGroup.resume_queue_size_.load(std::memory_order_relaxed) > 0) {
                std::array<Task, 100> task_bulk;
                cnt =
                    threadGroup.resume_queue_.try_dequeue_bulk(task_bulk.begin(), task_bulk.size());
                threadGroup.resume_queue_size_.fetch_sub(cnt);
                for (size_t idx = 0; idx < cnt; ++idx) {
                    _localWorkQueue.emplace_back(std::move(task_bulk[idx]));
                }
                // if (cnt > 0) {
                //     MONGO_LOG(1) << "get resume task";
                // }
            }

            if (cnt == 0 && threadGroup.task_queue_size_.load(std::memory_order_relaxed) > 0) {
                std::array<Task, 100> task_bulk;
                cnt = threadGroup.task_queue_.try_dequeue_bulk(task_bulk.begin(), task_bulk.size());
                threadGroup.task_queue_size_.fetch_sub(cnt);
                for (size_t idx = 0; idx < cnt; ++idx) {
                    _localWorkQueue.emplace_back(std::move(task_bulk[idx]));
                }
                // if (cnt > 0) {
                //     MONGO_LOG(1) << "get normal task";
                // }
            }

            if (cnt == 0) {
                continue;
            }

            while (!_localWorkQueue.empty() && _stillRunning.load(std::memory_order_relaxed)) {
                // _localRecursionDepth = 1;
                // MONGO_LOG(1) << "thread " << threadGroupId << " do task";
                setThreadName(threadNameSD);
                _localWorkQueue.front()();
                // MONGO_LOG(1) << "thread " << threadGroupId << " do task done";
                _localWorkQueue.pop_front();
            }
        }

        LOG(3) << "Exiting worker thread in " << _name << " service executor";
    });
}


Status ServiceExecutorCoroutine::shutdown(Milliseconds timeout) {
    LOG(3) << "Shutting down coroutine executor";

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
                 "coroutine executor couldn't shutdown all worker threads within time limit.");
}

Status ServiceExecutorCoroutine::schedule(Task task,
                                          ScheduleFlags flags,
                                          ServiceExecutorTaskName taskName) {
    return schedule(task, flags, taskName, 0);
}

Status ServiceExecutorCoroutine::schedule(Task task,
                                          ScheduleFlags flags,
                                          ServiceExecutorTaskName taskName,
                                          uint16_t thd_group_id) {
    MONGO_LOG(1) << "schedule with group id: " << thd_group_id;
    if (!_stillRunning.load()) {
        return Status{ErrorCodes::ShutdownInProgress, "Executor is not running"};
    }

    // if (!_localWorkQueue.empty()) {
    //     MONGO_LOG(0) << "here?";
    //     /*
    //      * In perf testing we found that yielding after running a each request produced
    //      * at 5% performance boost in microbenchmarks if the number of worker threads
    //      * was greater than the number of available cores.
    //      */
    //     if (flags & ScheduleFlags::kMayYieldBeforeSchedule) {
    //         if ((_localThreadIdleCounter++ & 0xf) == 0) {
    //             markThreadIdle();
    //         }
    //     }

    //     // Execute task directly (recurse) if allowed by the caller as it produced better
    //     // performance in testing. Try to limit the amount of recursion so we don't blow up the
    //     // stack, even though this shouldn't happen with this executor that uses blocking network
    //     // I/O.
    //     if ((flags & ScheduleFlags::kMayRecurse) &&
    //         (_localRecursionDepth < reservedServiceExecutorRecursionLimit.loadRelaxed())) {
    //         ++_localRecursionDepth;
    //         task();
    //     } else {
    //         _localWorkQueue.emplace_back(std::move(task));
    //     }
    //     return Status::OK();
    // }

    _threadGroups[thd_group_id].EnqueueTask(std::move(task));

    return Status::OK();
}

std::function<void()> ServiceExecutorCoroutine::CoroutineResumeFunctor(uint16_t thd_group_id,
                                                                       Task task) {
    assert(thd_group_id < _threadGroups.size());
    return [thd_group = &_threadGroups[thd_group_id], tsk = std::move(task)]() {
        thd_group->ResumeTask(std::move(tsk));
    };
}

void ServiceExecutorCoroutine::appendStats(BSONObjBuilder* bob) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    *bob << kExecutorLabel << kExecutorName << kThreadsRunning
         << static_cast<int>(_numRunningWorkerThreads.loadRelaxed()) << kReadyThreads;
    //  << static_cast<int>(_numReadyThreads) << kStartingThreads
    //  << static_cast<int>(_numStartingThreads);
}

}  // namespace transport
}  // namespace mongo
