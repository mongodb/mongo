#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <stdint.h>

#include "mongo/base/status.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/service_executor_task_names.h"

#include "mongo/db/modules/monograph/tx_service/include/moodycamelqueue.h"
#include <string_view>

namespace mongo {
namespace transport {

class ThreadGroup {
    friend class ServiceExecutorCoroutine;
    using Task = std::function<void()>;

public:
    void enqueueTask(Task task);
    void resumeTask(Task task);

    void notifyIfAsleep();

    /**
     * @brief Called by the thread bound to this thread group.
     */
    void trySleep();

    void terminate();

    /*
     * Elapsed 1 second
     */
    void tick();

private:
    bool isBusy() const;

    moodycamel::ConcurrentQueue<Task> _taskQueue;
    std::atomic<size_t> _taskQueueSize{0};
    moodycamel::ConcurrentQueue<Task> _resumeQueue;
    std::atomic<size_t> _resumeQueueSize{0};

    std::atomic<bool> _isSleep{false};
    std::mutex _sleepMutex;
    std::condition_variable _sleepCV;
    std::atomic<bool> _isTerminated{false};
    uint16_t _ongoingCoroutineCnt{0};

    std::atomic<uint64_t> _tickCnt{0};
    static constexpr uint64_t kTrySleepTimeOut = 5;
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
class ServiceExecutorCoroutine final : public ServiceExecutor {
public:
    explicit ServiceExecutorCoroutine(ServiceContext* ctx, size_t reservedThreads = 1);

    Status start() override;

    Status schedule(Task task, ScheduleFlags flags, ServiceExecutorTaskName taskName) override;
    Status schedule(Task task,
                    ScheduleFlags flags,
                    ServiceExecutorTaskName taskName,
                    uint16_t threadGroupId) override;


    Status shutdown(Milliseconds timeout) override;

    Mode transportMode() const override {
        return Mode::kAsynchronous;
    }
    std::function<void()> coroutineResumeFunctor(uint16_t threadGroupId, Task task) override;
    void ongoingCoroutineCountUpdate(uint16_t threadGroupId, int delta) override;
    void appendStats(BSONObjBuilder* bob) const override;

private:
    Status _startWorker(uint16_t groupId);

    // static thread_local std::deque<Task> _localWorkQueue;
    // static thread_local int _localRecursionDepth;
    // static thread_local int64_t _localThreadIdleCounter;

    static constexpr size_t kTaskBatchSize{100};

    std::atomic<bool> _stillRunning{false};

    mutable stdx::mutex _mutex;
    stdx::condition_variable _threadWakeup;
    stdx::condition_variable _shutdownCondition;

    AtomicUInt32 _numRunningWorkerThreads{0};

    constexpr static std::string_view _name{"coroutine"};
    const size_t _reservedThreads;

    std::vector<ThreadGroup> _threadGroups;
    std::thread _backgroundTimeService;
};

}  // namespace transport
}  // namespace mongo