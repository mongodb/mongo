

#pragma once

#include <atomic>
#include <deque>
#include <functional>

#include "mongo/base/status.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/service_executor_task_names.h"

#include "mongo/db/modules/monograph/tx_service/include/moodycamelqueue.h"

namespace mongo {
namespace transport {

class ThreadGroup {
    using Task = std::function<void()>;

public:
    void EnqueueTask(Task task) {
        task_queue_size_.fetch_add(1, std::memory_order_relaxed);
        task_queue_.enqueue(std::move(task));

        NotifyIfAsleep();
    }

    void ResumeTask(Task task) {
        resume_queue_size_.fetch_add(1, std::memory_order_relaxed);
        resume_queue_.enqueue(std::move(task));

        NotifyIfAsleep();
    }

    void NotifyIfAsleep() {
        if (_is_sleep.load(std::memory_order_relaxed)) {
            std::unique_lock<std::mutex> lk(_sleep_mux);
            _sleep_cv.notify_one();
        }
    }

    /**
     * @brief Called by the thread bound to this thread group.
     *
     */
    void TrySleep() {
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

    void Terminate() {
        _is_terminated.store(true, std::memory_order_relaxed);
        std::unique_lock<std::mutex> lk(_sleep_mux);
        _sleep_cv.notify_one();
    }

private:
    bool IsIdle() const {
        return task_queue_size_.load(std::memory_order_relaxed) == 0 &&
            resume_queue_size_.load(std::memory_order_relaxed) == 0 &&
            !_is_terminated.load(std::memory_order_relaxed);
    }

    moodycamel::ConcurrentQueue<Task> task_queue_;
    std::atomic<size_t> task_queue_size_{0};
    moodycamel::ConcurrentQueue<Task> resume_queue_;
    std::atomic<size_t> resume_queue_size_{0};

    std::atomic<bool> _is_sleep{false};
    std::mutex _sleep_mux;
    std::condition_variable _sleep_cv;
    std::atomic<bool> _is_terminated{false};

    friend class ServiceExecutorReserved;
};

/**
 * The reserved service executor emulates a thread per connection.
 * Each connection has its own worker thread where jobs get scheduled.
 *
 * The executor will start reservedThreads on start, and create a new thread every time it
 * starts a new thread, ensuring there are always reservedThreads available for work - this
 * means that even when you hit the NPROC ulimit, there will still be threads ready to
 * accept work. When threads exit, they will go back to waiting for work if there are fewer
 * than reservedThreads available.
 */
class ServiceExecutorReserved final : public ServiceExecutor {
public:
    explicit ServiceExecutorReserved(ServiceContext* ctx, const std::string& name, size_t reservedThreads);

    Status start() override;

    Status schedule(Task task, ScheduleFlags flags, ServiceExecutorTaskName taskName) override;
    Status schedule(Task task,
                    ScheduleFlags flags,
                    ServiceExecutorTaskName taskName,
                    uint16_t thd_group_id) override;

    Status shutdown(Milliseconds timeout) override;

    Mode transportMode() const override {
        return Mode::kSynchronous;
    }

    void appendStats(BSONObjBuilder* bob) const override;

    std::function<void()> CoroutineResumeFunctor(uint16_t thd_group_id, Task task) override {
        assert(thd_group_id < _threadGroups.size());
        return [thd_group = &_threadGroups[thd_group_id], tsk = std::move(task)]() {
            thd_group->ResumeTask(std::move(tsk));
        };
    }

private:
    Status _startWorker(uint16_t group_id);

    static thread_local std::deque<Task> _localWorkQueue;
    static thread_local int _localRecursionDepth;
    static thread_local int64_t _localThreadIdleCounter;

     std::atomic<bool> _stillRunning{false};

    mutable stdx::mutex _mutex;
    stdx::condition_variable _threadWakeup;
    stdx::condition_variable _shutdownCondition;

    std::deque<Task> _readyTasks;

    AtomicUInt32 _numRunningWorkerThreads{0};
    size_t _numReadyThreads{0};
    size_t _numStartingThreads{0};

    const std::string _name;
    const size_t _reservedThreads;

    std::vector<ThreadGroup> _threadGroups;
};

}  // namespace transport
}  // namespace mongo
