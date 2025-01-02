#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kExecutor;

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <tuple>

#include "mongo/base/string_data.h"
#include "mongo/db/server_parameters.h"
#include "mongo/transport/service_entry_point_utils.h"
#include "mongo/transport/service_executor_coroutine.h"
#include "mongo/transport/service_executor_task_names.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/log.h"

#ifndef EXT_TX_PROC_ENABLED
#define EXT_TX_PROC_ENABLED
#endif

namespace mongo {

extern thread_local int16_t localThreadId;
extern std::function<std::pair<std::function<void()>, std::function<void(int16_t)>>(int16_t)>
    getTxServiceFunctors;

namespace transport {
// namespace {

// // Tasks scheduled with MayRecurse may be called recursively if the recursion depth is below this
// // value.
// MONGO_EXPORT_SERVER_PARAMETER(reservedServiceExecutorRecursionLimit, int, 8);

// constexpr auto kThreadsRunning = "threadsRunning"_sd;
// constexpr auto kExecutorLabel = "executor"_sd;
// constexpr auto kExecutorName = "reserved"_sd;
// constexpr auto kReadyThreads = "readyThreads"_sd;
// constexpr auto kStartingThreads = "startingThreads"_sd;
// }  // namespace


void ThreadGroup::enqueueTask(Task task) {
    _taskQueueSize.fetch_add(1, std::memory_order_relaxed);
    _taskQueue.enqueue(std::move(task));

    notifyIfAsleep();
}

void ThreadGroup::resumeTask(Task task) {
    _resumeQueueSize.fetch_add(1, std::memory_order_relaxed);
    _resumeQueue.enqueue(std::move(task));

    notifyIfAsleep();
}

void ThreadGroup::notifyIfAsleep() {
    if (_isSleep.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lk(_sleepMutex);
        _sleepCV.notify_one();
    }
}

void ThreadGroup::setTxServiceFunctors(int16_t id) {
    std::tie(_txProcessorExec, _updateExtProc) = getTxServiceFunctors(id);
}

bool ThreadGroup::isBusy() const {
    return (_ongoingCoroutineCnt > 0) || (_taskQueueSize.load(std::memory_order_relaxed) > 0) ||
        (_resumeQueueSize.load(std::memory_order_relaxed) > 0);
}

void ThreadGroup::trySleep() {
    // If there are tasks in the , does not sleep.
    // if (isBusy()) {
    //     return;
    // }

    // MONGO_LOG(0) << "idle";
    // wait for kTrySleepTimeOut at most
    // _tickCnt.store(0, std::memory_order_release);
    // while (_tickCnt.load(std::memory_order_relaxed) < kTrySleepTimeOut) {
    //     if (isBusy()) {
    //         return;
    //     }
    // }

    // Sets the sleep flag before entering the critical section. std::memory_order_relaxed is
    // good enough, because the following mutex ensures that this instruction happens before the
    // critical section
    _isSleep.store(true, std::memory_order_relaxed);

    std::unique_lock<std::mutex> lk(_sleepMutex);

    // Double checkes again in the critical section before going to sleep. If additional tasks
    // are enqueued, does not sleep.
    if (isBusy()) {
        _isSleep.store(false, std::memory_order_relaxed);
        return;
    }

    MONGO_LOG(0) << "sleep";
#ifdef EXT_TX_PROC_ENABLED
    _updateExtProc(-1);
#endif
    _sleepCV.wait(lk, [this] { return isBusy(); });

    // Woken up from sleep.
#ifdef EXT_TX_PROC_ENABLED
    _updateExtProc(1);
#endif
    _isSleep.store(false, std::memory_order_relaxed);
}

void ThreadGroup::terminate() {
    _isTerminated.store(true, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lk(_sleepMutex);
    _sleepCV.notify_one();
}


// thread_local std::deque<ServiceExecutor::Task> ServiceExecutorCoroutine::_localWorkQueue = {};
// thread_local int ServiceExecutorCoroutine::_localRecursionDepth = 0;
// thread_local int64_t ServiceExecutorCoroutine::_localThreadIdleCounter = 0;

ServiceExecutorCoroutine::ServiceExecutorCoroutine(ServiceContext* ctx, size_t reservedThreads)
    : _reservedThreads(reservedThreads), _threadGroups(reservedThreads) {}

Status ServiceExecutorCoroutine::start() {
    MONGO_LOG(0) << "ServiceExecutorCoroutine::start";
    {
        // stdx::unique_lock<stdx::mutex> lk(_mutex);
        _stillRunning.store(true, std::memory_order_release);
    }

    for (size_t i = 0; i < _reservedThreads; i++) {
        auto status = _startWorker(static_cast<int16_t>(i));
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status ServiceExecutorCoroutine::_startWorker(int16_t groupId) {
    MONGO_LOG(0) << "Starting new worker thread for " << _name << " service executor. "
                 << " group id: " << groupId;

    return launchServiceWorkerThread([this, threadGroupId = groupId] {
        while (!_stillRunning.load(std::memory_order_acquire)) {
        }
        localThreadId = threadGroupId;

        std::string threadName("thread_group_" + std::to_string(threadGroupId));
        // std::string threadName("thread_group");
        StringData threadNameSD(threadName);
        setThreadName(threadNameSD);

        // std::unique_lock<stdx::mutex> lk(_mutex);
        // _numRunningWorkerThreads.addAndFetch(1);
        // auto numRunningGuard = MakeGuard([&] {
        //     _numRunningWorkerThreads.subtractAndFetch(1);
        //     _shutdownCondition.notify_one();
        // });
        // lk.unlock();

        ThreadGroup& threadGroup = _threadGroups[threadGroupId];

#ifdef EXT_TX_PROC_ENABLED
        threadGroup.setTxServiceFunctors(threadGroupId);
        MONGO_LOG(0) << "threadGroup._updateExtProc(1)";
        threadGroup._updateExtProc(1);
#endif
        std::array<Task, kTaskBatchSize> taskBulk;
        size_t idleCnt = 0;
        std::chrono::steady_clock::time_point idleStartTime;
        while (_stillRunning.load(std::memory_order_relaxed)) {
            if (!_stillRunning.load(std::memory_order_relaxed)) {
                break;
            }

            size_t cnt = 0;
            // process resume task
            if (threadGroup._resumeQueueSize.load(std::memory_order_relaxed) > 0) {
                cnt = threadGroup._resumeQueue.try_dequeue_bulk(taskBulk.begin(), taskBulk.size());
                threadGroup._resumeQueueSize.fetch_sub(cnt);
                for (size_t i = 0; i < cnt; ++i) {
                    setThreadName(threadNameSD);
                    taskBulk[i]();
                }
            }

            // process normal task
            if (cnt == 0 && threadGroup._taskQueueSize.load(std::memory_order_relaxed) > 0) {
                cnt = threadGroup._taskQueue.try_dequeue_bulk(taskBulk.begin(), taskBulk.size());
                threadGroup._taskQueueSize.fetch_sub(cnt);
                for (size_t i = 0; i < cnt; ++i) {
                    setThreadName(threadNameSD);
                    taskBulk[i]();
                }
            }
#ifdef EXT_TX_PROC_ENABLED
            // process as a TxProcessor
            (threadGroup._txProcessorExec)();
#endif
            if (cnt == 0) {
                if (idleCnt == 0) {
                    idleStartTime = std::chrono::steady_clock::now();
                    MONGO_LOG(3) << "idleStartTime " << idleStartTime.time_since_epoch().count();
                }
                idleCnt++;
                if ((idleCnt & kIdleCycle) == 0) {
                    // check timeout
                    auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - idleStartTime)
                                        .count();
                    if (interval > kIdleTimeoutMs) {
                        threadGroup.trySleep();
                    }
                }
            } else {
                idleCnt = 0;
            }
        }

        LOG(0) << "Exiting worker thread in " << _name << " service executor";
    });
}


Status ServiceExecutorCoroutine::shutdown(Milliseconds timeout) {
    LOG(0) << "Shutting down coroutine executor";

    // stdx::unique_lock<stdx::mutex> lock(_mutex);
    _stillRunning.store(false, std::memory_order_relaxed);
    // _threadWakeup.notify_all();
    // if (_backgroundTimeService.joinable()) {
    //     _backgroundTimeService.join();
    // }

    for (ThreadGroup& thd_group : _threadGroups) {
        thd_group.terminate();
    }

    // bool result = _shutdownCondition.wait_for(lock, timeout.toSystemDuration(), [this]() {
    //     return _numRunningWorkerThreads.load() == 0;
    // });

    return Status::OK();
    // return result
    //     ? Status::OK()
    //     : Status(ErrorCodes::Error::ExceededTimeLimit,
    //              "coroutine executor couldn't shutdown all worker threads within time limit.");
}

Status ServiceExecutorCoroutine::schedule(Task task,
                                          ScheduleFlags flags,
                                          ServiceExecutorTaskName taskName) {
    return schedule(task, flags, taskName, 0);
}

Status ServiceExecutorCoroutine::schedule(Task task,
                                          ScheduleFlags flags,
                                          ServiceExecutorTaskName taskName,
                                          uint16_t threadGroupId) {
    MONGO_LOG(1) << "schedule with group id: " << threadGroupId;
    if (!_stillRunning.load(std::memory_order_relaxed)) {
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

    _threadGroups[threadGroupId].enqueueTask(std::move(task));

    return Status::OK();
}

std::function<void()> ServiceExecutorCoroutine::coroutineResumeFunctor(uint16_t threadGroupId,
                                                                       Task task) {
    assert(threadGroupId < _threadGroups.size());
    return [thd_group = &_threadGroups[threadGroupId], tsk = std::move(task)]() {
        thd_group->resumeTask(std::move(tsk));
    };
}

void ServiceExecutorCoroutine::ongoingCoroutineCountUpdate(uint16_t threadGroupId, int delta) {
    _threadGroups[threadGroupId]._ongoingCoroutineCnt += delta;
}

void ServiceExecutorCoroutine::appendStats(BSONObjBuilder* bob) const {
    // stdx::lock_guard<stdx::mutex> lk(_mutex);
    // *bob << kExecutorLabel << kExecutorName << kThreadsRunning
    //      << static_cast<int>(_numRunningWorkerThreads.loadRelaxed()) << kReadyThreads;
    //  << static_cast<int>(_numReadyThreads) << kStartingThreads
    //  << static_cast<int>(_numStartingThreads);
}

}  // namespace transport
}  // namespace mongo
