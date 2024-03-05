#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string_view>

#include "mongo/base/status.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/service_executor_task_names.h"

#include "mongo/db/modules/monograph/tx_service/include/moodycamelqueue.h"



namespace mongo::transport {

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

    void setTxServiceFunctors(int16_t id);

private:
    bool isBusy() const;

    // uint16_t id;

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

    std::function<void()> _txProcessorExec;
    std::function<void(int16_t)> _updateExtProc;
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
    Status _startWorker(int16_t groupId);

    // static thread_local std::deque<Task> _localWorkQueue;
    // static thread_local int _localRecursionDepth;
    // static thread_local int64_t _localThreadIdleCounter;

    std::atomic<bool> _stillRunning{false};

    mutable stdx::mutex _mutex;
    stdx::condition_variable _threadWakeup;
    stdx::condition_variable _shutdownCondition;

    AtomicUInt32 _numRunningWorkerThreads{0};

    const size_t _reservedThreads;

    std::vector<ThreadGroup> _threadGroups;
    std::thread _backgroundTimeService;

    constexpr static std::string_view _name{"coroutine"};
    constexpr static size_t kTaskBatchSize{100};
    constexpr static uint32_t kIdleCycle = (1 << 10) - 1;  // 2^n-1
    constexpr static uint32_t kIdleTimeoutMs = 1000;
};

} // namespace mongo::transport
