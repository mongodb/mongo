/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/transport/service_executor_adaptive.h"

#include <array>
#include <random>

#include "mongo/db/server_parameters.h"
#include "mongo/transport/service_entry_point_utils.h"
#include "mongo/transport/service_executor_task_names.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/duration.h"
#include "mongo/util/log.h"
#include "mongo/util/net/thread_idle_callback.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stringutils.h"

#include <asio.hpp>

namespace mongo {
namespace transport {
namespace {
// The executor will always keep this many number of threads around. If the value is -1,
// (the default) then it will be set to number of cores / 2.
MONGO_EXPORT_SERVER_PARAMETER(adaptiveServiceExecutorReservedThreads, int, -1);

// Each worker thread will allow ASIO to run for this many milliseconds before checking
// whether it should exit
MONGO_EXPORT_SERVER_PARAMETER(adaptiveServiceExecutorRunTimeMillis, int, 5000);

// The above parameter will be offset of some random value between -runTimeJitters/
// +runTimeJitters so that not all threads are starting/stopping execution at the same time
MONGO_EXPORT_SERVER_PARAMETER(adaptiveServiceExecutorRunTimeJitterMillis, int, 500);

// This is the maximum amount of time the controller thread will sleep before doing any
// stuck detection
MONGO_EXPORT_SERVER_PARAMETER(adaptiveServiceExecutorStuckThreadTimeoutMillis, int, 250);

// The maximum allowed latency between when a task is scheduled and a thread is started to
// service it.
MONGO_EXPORT_SERVER_PARAMETER(adaptiveServiceExecutorMaxQueueLatencyMicros, int, 500);

// Threads will exit themselves if they spent less than this percentage of the time they ran
// doing actual work.
MONGO_EXPORT_SERVER_PARAMETER(adaptiveServiceExecutorIdlePctThreshold, int, 60);

// Tasks scheduled with MayRecurse may be called recursively if the recursion depth is below this
// value.
MONGO_EXPORT_SERVER_PARAMETER(adaptiveServiceExecutorRecursionLimit, int, 8);

constexpr auto kTotalQueued = "totalQueued"_sd;
constexpr auto kTotalExecuted = "totalExecuted"_sd;
constexpr auto kTotalTimeExecutingUs = "totalTimeExecutingMicros"_sd;
constexpr auto kTotalTimeRunningUs = "totalTimeRunningMicros"_sd;
constexpr auto kTotalTimeQueuedUs = "totalTimeQueuedMicros"_sd;
constexpr auto kThreadsInUse = "threadsInUse"_sd;
constexpr auto kThreadsRunning = "threadsRunning"_sd;
constexpr auto kThreadsPending = "threadsPending"_sd;
constexpr auto kExecutorLabel = "executor"_sd;
constexpr auto kExecutorName = "adaptive"_sd;
constexpr auto kStuckDetection = "stuckThreadsDetected"_sd;
constexpr auto kStarvation = "starvation"_sd;
constexpr auto kReserveMinimum = "belowReserveMinimum"_sd;
constexpr auto kBecauseOfError = "replacingCrashedThreads"_sd;
constexpr auto kThreadReasons = "threadCreationCauses"_sd;

int64_t ticksToMicros(TickSource::Tick ticks, TickSource* tickSource) {
    invariant(tickSource->getTicksPerSecond() >= 1000000);
    static const auto ticksPerMicro = tickSource->getTicksPerSecond() / 1000000;
    return ticks / ticksPerMicro;
}

struct ServerParameterOptions : public ServiceExecutorAdaptive::Options {
    int reservedThreads() const final {
        int value = adaptiveServiceExecutorReservedThreads.load();
        if (value == -1) {
            value = ProcessInfo::getNumAvailableCores() / 2;
            value = std::max(value, 2);
            adaptiveServiceExecutorReservedThreads.store(value);
            log() << "No thread count configured for executor. Using number of cores / 2: "
                  << value;
        }
        return value;
    }

    Milliseconds workerThreadRunTime() const final {
        return Milliseconds{adaptiveServiceExecutorRunTimeMillis.load()};
    }

    int runTimeJitter() const final {
        return adaptiveServiceExecutorRunTimeJitterMillis.load();
    }

    Milliseconds stuckThreadTimeout() const final {
        return Milliseconds{adaptiveServiceExecutorStuckThreadTimeoutMillis.load()};
    }

    Microseconds maxQueueLatency() const final {
        static Nanoseconds minTimerResolution = getMinimumTimerResolution();
        Microseconds value{adaptiveServiceExecutorMaxQueueLatencyMicros.load()};
        if (value < minTimerResolution) {
            log() << "Target MaxQueueLatencyMicros (" << value
                  << ") is less than minimum timer resolution of OS (" << minTimerResolution
                  << "). Using " << minTimerResolution;
            value = duration_cast<Microseconds>(minTimerResolution) + Microseconds{1};
            adaptiveServiceExecutorMaxQueueLatencyMicros.store(value.count());
        }
        return value;
    }

    int idlePctThreshold() const final {
        return adaptiveServiceExecutorIdlePctThreshold.load();
    }

    int recursionLimit() const final {
        return adaptiveServiceExecutorRecursionLimit.load();
    }
};

}  // namespace

thread_local ServiceExecutorAdaptive::ThreadState* ServiceExecutorAdaptive::_localThreadState =
    nullptr;

ServiceExecutorAdaptive::ServiceExecutorAdaptive(ServiceContext* ctx,
                                                 std::shared_ptr<asio::io_context> ioCtx)
    : ServiceExecutorAdaptive(ctx, std::move(ioCtx), stdx::make_unique<ServerParameterOptions>()) {}

ServiceExecutorAdaptive::ServiceExecutorAdaptive(ServiceContext* ctx,
                                                 std::shared_ptr<asio::io_context> ioCtx,
                                                 std::unique_ptr<Options> config)
    : _ioContext(std::move(ioCtx)),
      _config(std::move(config)),
      _tickSource(ctx->getTickSource()),
      _lastScheduleTimer(_tickSource) {}

ServiceExecutorAdaptive::~ServiceExecutorAdaptive() {
    invariant(!_isRunning.load());
}

Status ServiceExecutorAdaptive::start() {
    invariant(!_isRunning.load());
    _isRunning.store(true);
    _controllerThread = stdx::thread(&ServiceExecutorAdaptive::_controllerThreadRoutine, this);
    for (auto i = 0; i < _config->reservedThreads(); i++) {
        _startWorkerThread(ThreadCreationReason::kReserveMinimum);
    }

    return Status::OK();
}

Status ServiceExecutorAdaptive::shutdown(Milliseconds timeout) {
    if (!_isRunning.load())
        return Status::OK();

    _isRunning.store(false);

    _scheduleCondition.notify_one();
    _controllerThread.join();

    stdx::unique_lock<stdx::mutex> lk(_threadsMutex);
    _ioContext->stop();
    bool result =
        _deathCondition.wait_for(lk, timeout.toSystemDuration(), [&] { return _threads.empty(); });

    return result
        ? Status::OK()
        : Status(ErrorCodes::Error::ExceededTimeLimit,
                 "adaptive executor couldn't shutdown all worker threads within time limit.");
}

Status ServiceExecutorAdaptive::schedule(ServiceExecutorAdaptive::Task task,
                                         ScheduleFlags flags,
                                         ServiceExecutorTaskName taskName) {
    auto scheduleTime = _tickSource->getTicks();
    auto pendingCounterPtr = (flags & kDeferredTask) ? &_deferredTasksQueued : &_tasksQueued;
    pendingCounterPtr->addAndFetch(1);

    if (!_isRunning.load()) {
        return {ErrorCodes::ShutdownInProgress, "Executor is not running"};
    }

    auto wrappedTask =
        [ this, task = std::move(task), scheduleTime, pendingCounterPtr, taskName, flags ] {
        pendingCounterPtr->subtractAndFetch(1);
        auto start = _tickSource->getTicks();
        _totalSpentQueued.addAndFetch(start - scheduleTime);

        _localThreadState->threadMetrics[static_cast<size_t>(taskName)]
            ._totalSpentQueued.addAndFetch(start - scheduleTime);

        if (_localThreadState->recursionDepth++ == 0) {
            _localThreadState->executing.markRunning();
            _threadsInUse.addAndFetch(1);
        }
        const auto guard = MakeGuard([this, start, taskName] {
            if (--_localThreadState->recursionDepth == 0) {
                _localThreadState->executingCurRun += _localThreadState->executing.markStopped();
                _threadsInUse.subtractAndFetch(1);
            }
            _totalExecuted.addAndFetch(1);
            _localThreadState->threadMetrics[static_cast<size_t>(taskName)]
                ._totalExecuted.addAndFetch(1);
        });

        TickTimer _localTimer(_tickSource);
        task();
        _localThreadState->threadMetrics[static_cast<size_t>(taskName)]
            ._totalSpentExecuting.addAndFetch(_localTimer.sinceStartTicks());

        if ((flags & ServiceExecutor::kMayYieldBeforeSchedule) &&
            (_localThreadState->markIdleCounter++ & 0xf)) {
            markThreadIdle();
        }
    };

    // Dispatching a task on the io_context will run the task immediately, and may run it
    // on the current thread (if the current thread is running the io_context right now).
    //
    // Posting a task on the io_context will run the task without recursion.
    //
    // If the task is allowed to recurse and we are not over the depth limit, dispatch it so it
    // can be called immediately and recursively.
    if ((flags & kMayRecurse) &&
        (_localThreadState->recursionDepth + 1 < _config->recursionLimit())) {
        _ioContext->dispatch(std::move(wrappedTask));
    } else {
        _ioContext->post(std::move(wrappedTask));
    }

    _lastScheduleTimer.reset();
    _totalQueued.addAndFetch(1);

    _accumulatedMetrics[static_cast<size_t>(taskName)]._totalQueued.addAndFetch(1);

    // Deferred tasks never count against the thread starvation avoidance. For other tasks, we
    // notify the controller thread that a task has been scheduled and we should monitor thread
    // starvation.
    if (_isStarved() && !(flags & kDeferredTask)) {
        _starvationCheckRequests.addAndFetch(1);
        _scheduleCondition.notify_one();
    }

    return Status::OK();
}

bool ServiceExecutorAdaptive::_isStarved() const {
    // If threads are still starting, then assume we won't be starved pretty soon, return false
    if (_threadsPending.load() > 0)
        return false;

    auto tasksQueued = _tasksQueued.load();
    // If there are no pending tasks, then we definitely aren't starved
    if (tasksQueued == 0)
        return false;

    // The available threads is the number that are running - the number that are currently
    // executing
    auto available = _threadsRunning.load() - _threadsInUse.load();

    return (tasksQueued > available);
}

/*
 * The pool of worker threads can become unhealthy in several ways, and the controller thread
 * tries to keep the pool healthy by starting new threads when it is:
 *
 * Stuck: All threads are running a long-running task that's waiting on a network event, but
 * there are no threads available to process network events. The thread pool cannot make progress
 * without intervention.
 *
 * Starved: All threads are saturated with tasks and new tasks are having to queue for longer
 * than the configured maxQueueLatency().
 *
 * Below reserve: An error has occurred and there are fewer threads than the reserved minimum.
 *
 * While the executor is running, it runs in a loop waiting to be woken up by schedule() or a
 * timeout to occur. When it wakes up, it ensures that:
 * - The thread pool is not stuck longer than the configured stuckThreadTimeout(). If it is, then
 *   start a new thread and wait to be woken up again (or time out again and redo stuck thread
 *   detection).
 * - The number of threads is >= the reservedThreads() value. If it isn't, then start as many
 *   threads as necessary.
 * - Checking for starvation when requested by schedule(), and starting new threads if the
 *   pool is saturated and is starved longer than the maxQueueLatency() after being woken up
 *   by schedule().
 */
void ServiceExecutorAdaptive::_controllerThreadRoutine() {
    stdx::mutex noopLock;
    setThreadName("worker-controller"_sd);

    // Setup the timers/timeout values for stuck thread detection.
    TickTimer sinceLastStuckThreadCheck(_tickSource);
    auto stuckThreadTimeout = _config->stuckThreadTimeout();

    // Get the initial values for our utilization percentage calculations
    auto getTimerTotals = [this]() {
        stdx::unique_lock<stdx::mutex> lk(_threadsMutex);
        auto first = _getThreadTimerTotal(ThreadTimer::kExecuting, lk);
        auto second = _getThreadTimerTotal(ThreadTimer::kRunning, lk);
        return std::make_pair(first, second);
    };

    TickSource::Tick lastSpentExecuting, lastSpentRunning;
    std::tie(lastSpentExecuting, lastSpentRunning) = getTimerTotals();

    while (_isRunning.load()) {
        // We want to wait for schedule() to wake us up, or for the stuck thread timeout to pass.
        // So the timeout is the current stuck thread timeout - the last time we did stuck thread
        // detection.
        auto timeout = stuckThreadTimeout - sinceLastStuckThreadCheck.sinceStart();

        bool maybeStarved = false;
        // If the timeout is less than a millisecond then don't bother to go to sleep to wait for
        // it, just do the stuck thread detection now.
        if (timeout > Milliseconds{0}) {
            stdx::unique_lock<decltype(noopLock)> scheduleLk(noopLock);
            int checkRequests = 0;
            maybeStarved = _scheduleCondition.wait_for(
                scheduleLk, timeout.toSystemDuration(), [this, &checkRequests] {
                    if (!_isRunning.load())
                        return false;
                    checkRequests = _starvationCheckRequests.load();
                    return (checkRequests > 0);
                });

            _starvationCheckRequests.subtractAndFetch(checkRequests);
        }

        // If the executor has stopped, then stop the controller altogether
        if (!_isRunning.load())
            break;

        if (sinceLastStuckThreadCheck.sinceStart() >= stuckThreadTimeout) {
            // Reset our timer so we know how long to sleep for the next time around;
            sinceLastStuckThreadCheck.reset();

            // Each call to schedule updates the last schedule ticks so we know the last time a
            // task was scheduled
            Milliseconds sinceLastSchedule = _lastScheduleTimer.sinceStart();

            // If the number of tasks executing is the number of threads running (that is all
            // threads are currently busy), and the last time a task was able to be scheduled was
            // longer than our wait timeout, then we can assume all threads are stuck and we should
            // start a new thread to unblock the pool.
            //
            if ((_threadsInUse.load() == _threadsRunning.load()) &&
                (sinceLastSchedule >= stuckThreadTimeout)) {
                // When the executor is stuck, we halve the stuck thread timeout to be more
                // aggressive the next time out unsticking the executor, and then start a new
                // thread to unblock the executor for now.
                stuckThreadTimeout /= 2;
                stuckThreadTimeout = std::max(Milliseconds{10}, stuckThreadTimeout);
                log() << "Detected blocked worker threads, "
                      << "starting new thread to unblock service executor. "
                      << "Stuck thread timeout now: " << stuckThreadTimeout;
                _startWorkerThread(ThreadCreationReason::kStuckDetection);

                // Since we've just started a worker thread, then we know that the executor isn't
                // starved, so just loop back around to wait for the next control event.
                continue;
            }

            // If the executor wasn't stuck, then we should back off our stuck thread timeout back
            // towards the configured value.
            auto newStuckThreadTimeout = stuckThreadTimeout + (stuckThreadTimeout / 2);
            newStuckThreadTimeout = std::min(_config->stuckThreadTimeout(), newStuckThreadTimeout);
            if (newStuckThreadTimeout != stuckThreadTimeout) {
                LOG(1) << "Increasing stuck thread timeout to " << newStuckThreadTimeout;
                stuckThreadTimeout = newStuckThreadTimeout;
            }
        }

        auto threadsRunning = _threadsRunning.load();
        if (threadsRunning < _config->reservedThreads()) {
            log() << "Starting " << _config->reservedThreads() - threadsRunning
                  << " to replenish reserved worker threads";
            while (_threadsRunning.load() < _config->reservedThreads()) {
                _startWorkerThread(ThreadCreationReason::kReserveMinimum);
            }
        }

        // If we were notified by schedule() to do starvation checking, then we first need to
        // calculate the overall utilization of the executor.
        if (maybeStarved) {

            // Get the difference between the amount of time the executor has spent waiting for/
            // running tasks since the last time we measured.
            TickSource::Tick spentExecuting, spentRunning;
            std::tie(spentExecuting, spentRunning) = getTimerTotals();
            auto diffExecuting = spentExecuting - lastSpentExecuting;
            auto diffRunning = spentRunning - lastSpentRunning;

            double utilizationPct;
            // If we spent zero time running then the executor was fully idle and our utilization
            // is zero percent
            if (spentRunning == 0 || diffRunning == 0)
                utilizationPct = 0.0;
            else {
                lastSpentExecuting = spentExecuting;
                lastSpentRunning = spentRunning;

                utilizationPct = diffExecuting / static_cast<double>(diffRunning);
                utilizationPct *= 100;
            }

            // If the utilization percentage is less than our threshold then we don't want to
            // do anything because the threads are not actually saturated with work.
            if (utilizationPct < _config->idlePctThreshold()) {
                continue;
            }
        }

        // While there are threads that are still starting up, wait for the max queue latency,
        // up to the current stuck thread timeout.
        do {
            stdx::this_thread::sleep_for(_config->maxQueueLatency().toSystemDuration());
        } while ((_threadsPending.load() > 0) &&
                 (sinceLastStuckThreadCheck.sinceStart() < stuckThreadTimeout));

        // If the number of pending tasks is greater than the number of running threads minus the
        // number of tasks executing (the number of free threads), then start a new worker to
        // avoid starvation.
        if (_isStarved()) {
            log() << "Starting worker thread to avoid starvation.";
            _startWorkerThread(ThreadCreationReason::kStarvation);
        }
    }
}

void ServiceExecutorAdaptive::_startWorkerThread(ThreadCreationReason reason) {
    stdx::unique_lock<stdx::mutex> lk(_threadsMutex);
    auto it = _threads.emplace(_threads.begin(), _tickSource);
    auto num = _threads.size();

    _threadsPending.addAndFetch(1);
    _threadsRunning.addAndFetch(1);
    _threadStartCounters[static_cast<size_t>(reason)] += 1;

    lk.unlock();

    const auto launchResult =
        launchServiceWorkerThread([this, num, it] { _workerThreadRoutine(num, it); });

    if (!launchResult.isOK()) {
        warning() << "Failed to launch new worker thread: " << launchResult;
        lk.lock();
        _threadsPending.subtractAndFetch(1);
        _threadsRunning.subtractAndFetch(1);
        _threadStartCounters[static_cast<size_t>(reason)] -= 1;
        _threads.erase(it);
    }
}

Milliseconds ServiceExecutorAdaptive::_getThreadJitter() const {
    static stdx::mutex jitterMutex;
    static std::default_random_engine randomEngine = [] {
        std::random_device seed;
        return std::default_random_engine(seed());
    }();

    auto jitterParam = _config->runTimeJitter();
    if (jitterParam == 0)
        return Milliseconds{0};

    std::uniform_int_distribution<> jitterDist(-jitterParam, jitterParam);

    stdx::lock_guard<stdx::mutex> lk(jitterMutex);
    auto jitter = jitterDist(randomEngine);
    if (jitter > _config->workerThreadRunTime().count())
        jitter = 0;

    return Milliseconds{jitter};
}

void ServiceExecutorAdaptive::_accumulateTaskMetrics(MetricsArray* outArray,
                                                     const MetricsArray& inputArray) const {
    for (auto it = inputArray.begin(); it != inputArray.end(); ++it) {
        auto taskName = static_cast<ServiceExecutorTaskName>(std::distance(inputArray.begin(), it));
        auto& output = outArray->at(static_cast<size_t>(taskName));

        output._totalSpentExecuting.addAndFetch(it->_totalSpentExecuting.load());
        output._totalSpentQueued.addAndFetch(it->_totalSpentQueued.load());
        output._totalExecuted.addAndFetch(it->_totalExecuted.load());
        output._totalQueued.addAndFetch(it->_totalQueued.load());
    }
}

void ServiceExecutorAdaptive::_accumulateAllTaskMetrics(
    MetricsArray* outputMetricsArray, const stdx::unique_lock<stdx::mutex>& lk) const {
    _accumulateTaskMetrics(outputMetricsArray, _accumulatedMetrics);
    for (auto& thread : _threads) {
        _accumulateTaskMetrics(outputMetricsArray, thread.threadMetrics);
    }
}

TickSource::Tick ServiceExecutorAdaptive::_getThreadTimerTotal(
    ThreadTimer which, const stdx::unique_lock<stdx::mutex>& lk) const {
    TickSource::Tick accumulator;
    switch (which) {
        case ThreadTimer::kRunning:
            accumulator = _pastThreadsSpentRunning.load();
            break;
        case ThreadTimer::kExecuting:
            accumulator = _pastThreadsSpentExecuting.load();
            break;
    }

    for (auto& thread : _threads) {
        switch (which) {
            case ThreadTimer::kRunning:
                accumulator += thread.running.totalTime();
                break;
            case ThreadTimer::kExecuting:
                accumulator += thread.executing.totalTime();
                break;
        }
    }

    return accumulator;
}

void ServiceExecutorAdaptive::_workerThreadRoutine(
    int threadId, ServiceExecutorAdaptive::ThreadList::iterator state) {
    _threadsPending.subtractAndFetch(1);
    _localThreadState = &(*state);
    {
        std::string threadName = str::stream() << "worker-" << threadId;
        setThreadName(threadName);
    }

    log() << "Started new database worker thread " << threadId;

    bool guardThreadsRunning = true;
    const auto guard = MakeGuard([this, &guardThreadsRunning, state] {
        if (guardThreadsRunning)
            _threadsRunning.subtractAndFetch(1);
        _pastThreadsSpentRunning.addAndFetch(state->running.totalTime());
        _pastThreadsSpentExecuting.addAndFetch(state->executing.totalTime());

        _accumulateTaskMetrics(&_accumulatedMetrics, state->threadMetrics);
        {
            stdx::lock_guard<stdx::mutex> lk(_threadsMutex);
            _threads.erase(state);
        }
        _deathCondition.notify_one();
    });

    auto jitter = _getThreadJitter();

    while (_isRunning.load()) {
        // We don't want all the threads to start/stop running at exactly the same time, so the
        // jitter setParameter adds/removes a random small amount of time to the runtime.
        Milliseconds runTime = _config->workerThreadRunTime() + jitter;
        dassert(runTime.count() > 0);

        // Reset ticksSpentExecuting timer
        state->executingCurRun = 0;

        try {
            asio::io_context::work work(*_ioContext);
            // If we're still "pending" only try to run one task, that way the controller will
            // know that it's okay to start adding threads to avoid starvation again.
            state->running.markRunning();
            _ioContext->run_for(runTime.toSystemDuration());

            // _ioContext->run_one() will return when all the scheduled handlers are completed, and
            // you must call restart() to call run_one() again or else it will return immediately.
            // In the case where the server has just started and there has been no work yet, this
            // means this loop will spin until the first client connect. This call to restart avoids
            // that.
            if (_ioContext->stopped())
                _ioContext->restart();
            // If an exception escaped from ASIO, then break from this thread and start a new one.
        } catch (std::exception& e) {
            log() << "Exception escaped worker thread: " << e.what()
                  << " Starting new worker thread.";
            _startWorkerThread(ThreadCreationReason::kError);
            break;
        } catch (...) {
            log() << "Unknown exception escaped worker thread. Starting new worker thread.";
            _startWorkerThread(ThreadCreationReason::kError);
            break;
        }
        auto spentRunning = state->running.markStopped();

        // If we spent less than our idle threshold actually running tasks then exit the thread.
        // This is a helper lambda to perform that calculation.
        const auto calculatePctExecuting = [&spentRunning, &state]() {
            // This time measurement doesn't include time spent running network callbacks,
            // so the threshold is lower than you'd expect.
            dassert(spentRunning < std::numeric_limits<double>::max());

            // First get the ratio of ticks spent executing to ticks spent running. We
            // expect this to be <= 1.0
            double executingToRunning = state->executingCurRun / static_cast<double>(spentRunning);

            // Multiply that by 100 to get the percentage of time spent executing tasks. We
            // expect this to be <= 100.
            executingToRunning *= 100;
            dassert(executingToRunning <= 100);

            return static_cast<int>(executingToRunning);
        };

        bool terminateThread = false;
        int pctExecuting;
        int runningThreads;

        // Make sure we don't terminate threads below the reserved threshold. As there can be
        // several worker threads concurrently in this terminate logic atomically reduce the threads
        // one by one to avoid racing using a lockless compare-and-swap loop where we retry if there
        // is contention on the atomic.
        do {
            runningThreads = _threadsRunning.load();

            if (runningThreads <= _config->reservedThreads()) {
                terminateThread = false;
                break;  // keep thread
            }

            if (!terminateThread) {
                pctExecuting = calculatePctExecuting();
                terminateThread = pctExecuting <= _config->idlePctThreshold();
            }
        } while (terminateThread &&
                 _threadsRunning.compareAndSwap(runningThreads, runningThreads - 1) !=
                     runningThreads);
        if (terminateThread) {
            log() << "Thread was only executing tasks " << pctExecuting << "% over the last "
                  << runTime << ". Exiting thread.";

            // Because we've already modified _threadsRunning, make sure the thread guard also
            // doesn't do it.
            guardThreadsRunning = false;
            break;
        }
    }
}

StringData ServiceExecutorAdaptive::_threadStartedByToString(
    ServiceExecutorAdaptive::ThreadCreationReason reason) {
    switch (reason) {
        case ThreadCreationReason::kStuckDetection:
            return kStuckDetection;
        case ThreadCreationReason::kStarvation:
            return kStarvation;
        case ThreadCreationReason::kReserveMinimum:
            return kReserveMinimum;
        case ThreadCreationReason::kError:
            return kBecauseOfError;
        default:
            MONGO_UNREACHABLE;
    }
}

void ServiceExecutorAdaptive::appendStats(BSONObjBuilder* bob) const {
    stdx::unique_lock<stdx::mutex> lk(_threadsMutex);
    *bob << kExecutorLabel << kExecutorName                                                //
         << kTotalQueued << _totalQueued.load()                                            //
         << kTotalExecuted << _totalExecuted.load()                                        //
         << kThreadsInUse << _threadsInUse.load()                                          //
         << kTotalTimeRunningUs                                                            //
         << ticksToMicros(_getThreadTimerTotal(ThreadTimer::kRunning, lk), _tickSource)    //
         << kTotalTimeExecutingUs                                                          //
         << ticksToMicros(_getThreadTimerTotal(ThreadTimer::kExecuting, lk), _tickSource)  //
         << kTotalTimeQueuedUs << ticksToMicros(_totalSpentQueued.load(), _tickSource)     //
         << kThreadsRunning << _threadsRunning.load()                                      //
         << kThreadsPending << _threadsPending.load();

    BSONObjBuilder threadStartReasons(bob->subobjStart(kThreadReasons));
    for (size_t i = 0; i < _threadStartCounters.size(); i++) {
        threadStartReasons << _threadStartedByToString(static_cast<ThreadCreationReason>(i))
                           << _threadStartCounters[i];
    }

    threadStartReasons.doneFast();

    BSONObjBuilder metricsByTask(bob->subobjStart("metricsByTask"));
    MetricsArray totalMetrics;
    _accumulateAllTaskMetrics(&totalMetrics, lk);
    lk.unlock();
    for (auto it = totalMetrics.begin(); it != totalMetrics.end(); ++it) {
        auto taskName =
            static_cast<ServiceExecutorTaskName>(std::distance(totalMetrics.begin(), it));
        auto taskNameString = taskNameToString(taskName);
        BSONObjBuilder subSection(metricsByTask.subobjStart(taskNameString));
        subSection << kTotalQueued << it->_totalQueued.load() << kTotalExecuted
                   << it->_totalExecuted.load() << kTotalTimeExecutingUs
                   << ticksToMicros(it->_totalSpentExecuting.load(), _tickSource)
                   << kTotalTimeQueuedUs
                   << ticksToMicros(it->_totalSpentQueued.load(), _tickSource);

        subSection.doneFast();
    }
    metricsByTask.doneFast();
}

}  // namespace transport
}  // namespace mongo
