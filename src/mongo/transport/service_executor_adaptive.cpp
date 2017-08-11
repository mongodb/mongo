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

#include <random>

#include "mongo/db/server_parameters.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/log.h"
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
MONGO_EXPORT_SERVER_PARAMETER(adaptiveServiceExecutorMaxQueueLatencyMicros, int, 50);

// Threads will exit themselves if they spent less than this percentage of the time they ran
// doing actual work.
MONGO_EXPORT_SERVER_PARAMETER(adaptiveServiceExecutorIdlePctThreshold, int, 60);

thread_local TickSource::Tick ticksSpentExecuting = 0;
thread_local TickSource::Tick ticksSpentScheduled = 0;
thread_local int tasksExecuted = 0;

constexpr auto kTotalQueued = "totalQueued"_sd;
constexpr auto kTotalExecuted = "totalExecuted"_sd;
constexpr auto kTasksQueued = "tasksQueued"_sd;
constexpr auto kTotalTimeExecutingUs = "totalTimeExecutingMicros"_sd;
constexpr auto kTotalTimeRunningUs = "totalTimeRunningMicros"_sd;
constexpr auto kTotalTimeQueuedUs = "totalTimeQueuedMicros"_sd;
constexpr auto kTasksExecuting = "tasksExecuting";
constexpr auto kThreadsRunning = "threadsRunning";
constexpr auto kThreadsPending = "threadsPending";
constexpr auto kExecutorLabel = "executor";
constexpr auto kExecutorName = "adaptive";

int64_t ticksToMicros(TickSource::Tick ticks, TickSource* tickSource) {
    invariant(tickSource->getTicksPerSecond() > 1000000);
    static const auto ticksPerMicro = tickSource->getTicksPerSecond() / 1000000;
    return ticks / ticksPerMicro;
}

struct ServerParameterOptions : public ServiceExecutorAdaptive::Options {
    int reservedThreads() const final {
        int value = adaptiveServiceExecutorReservedThreads.load();
        if (value == -1) {
            ProcessInfo pi;
            value = pi.getNumAvailableCores().value_or(pi.getNumCores()) / 2;
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
        return Microseconds{adaptiveServiceExecutorMaxQueueLatencyMicros.load()};
    }

    int idlePctThreshold() const final {
        return adaptiveServiceExecutorIdlePctThreshold.load();
    }
};


}  // namespace

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
        _startWorkerThread();
    }

    return Status::OK();
}

Status ServiceExecutorAdaptive::shutdown() {
    if (!_isRunning.load())
        return Status::OK();

    _isRunning.store(false);

    _scheduleCondition.notify_one();
    _controllerThread.join();

    stdx::unique_lock<stdx::mutex> lk(_threadsMutex);
    _ioContext->stop();
    _deathCondition.wait(lk, [&] { return _threads.empty(); });

    return Status::OK();
}

Status ServiceExecutorAdaptive::schedule(ServiceExecutorAdaptive::Task task, ScheduleFlags flags) {
    auto scheduleTime = _tickSource->getTicks();
    auto pending = _tasksQueued.addAndFetch(1);

    _ioContext->post([ this, task = std::move(task), scheduleTime ] {
        _tasksQueued.subtractAndFetch(1);
        auto start = _tickSource->getTicks();
        ticksSpentScheduled += (start - scheduleTime);
        _tasksExecuting.addAndFetch(1);

        const auto guard = MakeGuard([this, start] {
            _tasksExecuting.subtractAndFetch(1);
            ++tasksExecuted;
            auto thisSpentExecuting = _tickSource->getTicks() - start;
            ticksSpentExecuting += thisSpentExecuting;
            _totalExecuted.addAndFetch(1);
        });

        task();
    });

    _lastScheduleTimer.reset();
    _totalQueued.addAndFetch(1);

    if (_isStarved(pending) && !(flags & DeferredTask)) {
        _scheduleCondition.notify_one();
    }

    return Status::OK();
}

bool ServiceExecutorAdaptive::_isStarved(int pending) const {
    // If threads are still starting, then assume we won't be starved pretty soon, return false
    if (_threadsPending.load() > 0)
        return false;

    if (pending == -1) {
        pending = _tasksQueued.load();
    }
    // If there are no pending tasks, then we definitely aren't starved
    if (pending == 0)
        return false;

    // The available threads is the number that are running - the number that are currently
    // executing
    auto available = _threadsRunning.load() - _tasksExecuting.load();
    if (pending <= available)
        return false;

    return (pending > available);
}

void ServiceExecutorAdaptive::_controllerThreadRoutine() {
    setThreadName("worker-controller"_sd);
    // The scheduleCondition needs a lock to wait on.
    stdx::mutex fakeMutex;
    stdx::unique_lock<stdx::mutex> fakeLk(fakeMutex);

    TickTimer sinceLastControlRound(_tickSource);
    TickSource::Tick lastSpentExecuting = _totalSpentExecuting.load();
    TickSource::Tick lastSpentRunning = _totalSpentRunning.load();

    while (_isRunning.load()) {
        // Make sure that the timer gets reset whenever this loop completes
        const auto timerResetGuard =
            MakeGuard([&sinceLastControlRound] { sinceLastControlRound.reset(); });

        _scheduleCondition.wait_for(fakeLk, _config->stuckThreadTimeout().toSystemDuration());

        // If the executor has stopped, then stop the controller altogether
        if (!_isRunning.load())
            break;

        double utilizationPct;
        {
            auto spentExecuting = _totalSpentExecuting.load();
            auto spentRunning = _totalSpentRunning.load();
            auto diffExecuting = spentExecuting - lastSpentExecuting;
            auto diffRunning = spentRunning - lastSpentRunning;

            // If no threads have run yet, then don't update anything
            if (spentRunning == 0 || diffRunning == 0)
                utilizationPct = 0.0;
            else {
                lastSpentExecuting = spentExecuting;
                lastSpentRunning = spentRunning;

                utilizationPct = diffExecuting / static_cast<double>(diffRunning);
                utilizationPct *= 100;
            }
        }

        // If the wait timed out then either the executor is idle or stuck
        if (sinceLastControlRound.sinceStart() >= _config->stuckThreadTimeout()) {
            // Each call to schedule updates the last schedule ticks so we know the last time a
            // task was scheduled
            Milliseconds sinceLastSchedule = _lastScheduleTimer.sinceStart();

            // If the number of tasks executing is the number of threads running (that is all
            // threads are currently busy), and the last time a task was able to be scheduled was
            // longer than our wait timeout, then we can assume all threads are stuck.
            //
            // In that case we should start the reserve number of threads so fully unblock the
            // thread pool.
            //
            if ((_tasksExecuting.load() == _threadsRunning.load()) &&
                (sinceLastSchedule >= _config->stuckThreadTimeout())) {
                log() << "Detected blocked worker threads, "
                      << "starting new reserve threads to unblock service executor";
                for (int i = 0; i < _config->reservedThreads(); i++) {
                    _startWorkerThread();
                }
            }
            continue;
        }

        auto threadsRunning = _threadsRunning.load();
        if (threadsRunning < _config->reservedThreads()) {
            log() << "Starting " << _config->reservedThreads() - threadsRunning
                  << " to replenish reserved worker threads";
            while (_threadsRunning.load() < _config->reservedThreads()) {
                _startWorkerThread();
            }
        }

        // If the utilization percentage is lower than our idle threshold, then the threads we
        // already have aren't saturated and we shouldn't consider adding new threads at this
        // time.
        if (utilizationPct < _config->idlePctThreshold()) {
            continue;
        }

        // While there are threads pending sleep for 50 microseconds (this is our thread latency
        // perf budget).
        //
        // If waiting for pending threads takes longer than the stuckThreadTimeout, then the
        // pending threads may be stuck and we should loop back around.
        do {
            stdx::this_thread::sleep_for(_config->maxQueueLatency().toSystemDuration());
        } while ((_threadsPending.load() > 0) &&
                 (sinceLastControlRound.sinceStart() < _config->stuckThreadTimeout()));


        // If the number of pending tasks is greater than the number of running threads minus the
        // number of tasks executing (the number of free threads), then start a new worker to
        // avoid starvation.
        if (_isStarved()) {
            log() << "Starting worker thread to avoid starvation.";
            _startWorkerThread();
        }
    }
}

void ServiceExecutorAdaptive::_startWorkerThread() {
    stdx::unique_lock<stdx::mutex> lk(_threadsMutex);
    auto it = _threads.emplace(_threads.begin());
    auto num = _threads.size();

    _threadsPending.addAndFetch(1);
    _threadsRunning.addAndFetch(1);
    *it = stdx::thread(&ServiceExecutorAdaptive::_workerThreadRoutine, this, num, it);
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
    if (jitter < _config->workerThreadRunTime().count())
        jitter = 0;

    return Milliseconds{jitter};
}

void ServiceExecutorAdaptive::_workerThreadRoutine(
    int threadId, ServiceExecutorAdaptive::ThreadList::iterator it) {

    {
        std::string threadName = str::stream() << "worker-" << threadId;
        setThreadName(threadName);
    }

    log() << "Starting new database worker thread " << threadId;

    // Whether a thread is "pending" reflects whether its had a chance to do any useful work.
    // When a thread is pending, it will only try to run one task through ASIO, and report back
    // as soon as possible so that the thread controller knows not to keep starting threads while
    // the threads it's already created are finishing starting up.
    bool stillPending = true;

    const auto guard = MakeGuard([this, &stillPending, it] {
        if (stillPending)
            _threadsPending.subtractAndFetch(1);
        _threadsRunning.subtractAndFetch(1);

        {
            stdx::lock_guard<stdx::mutex> lk(_threadsMutex);
            it->detach();
            _threads.erase(it);
        }
        _deathCondition.notify_one();
    });

    auto jitter = _getThreadJitter();

    while (_isRunning.load()) {
        // We don't want all the threads to start/stop running at exactly the same time, so the
        // jitter setParameter adds/removes a random small amount of time to the runtime.
        Milliseconds runTime = _config->workerThreadRunTime() + jitter;
        dassert(runTime.count() > 0);

        size_t handlersRun;
        // Reset our thread-local counters
        tasksExecuted = 0;
        ticksSpentExecuting = 0;
        ticksSpentScheduled = 0;

        auto startRun = _tickSource->getTicks();
        try {
            asio::io_context::work work(*_ioContext);
            // If we're still "pending" only try to run one task, that way the controller will
            // know that it's okay to start adding threads to avoid starvation again.
            if (stillPending) {
                handlersRun = _ioContext->run_one_for(runTime.toSystemDuration());
            } else {  // Otherwise, just run for the full run period
                handlersRun = _ioContext->run_for(runTime.toSystemDuration());
            }

            // _ioContext->run_one() will return when all the scheduled handlers are completed, and
            // you must call restart() to call run_one() again or else it will return immediately.
            // In the case where the server has just started and there has been no work yet, this
            // means this loop will spin until the first client connect. Thsi call to restart avoids
            // that.
            if (_ioContext->stopped())
                _ioContext->restart();
            // If an exceptione escaped from ASIO, then break from this thread and start a new one.
        } catch (std::exception& e) {
            log() << "Exception escaped worker thread: " << e.what()
                  << " Starting new worker thread.";
            _startWorkerThread();
            break;
        } catch (...) {
            log() << "Unknown exception escaped worker thread. Starting new worker thread.";
            _startWorkerThread();
            break;
        }
        auto spentRunning = _tickSource->getTicks() - startRun;
        _totalSpentRunning.addAndFetch(spentRunning);
        _totalSpentExecuting.addAndFetch(ticksSpentExecuting);
        _totalSpentScheduled.addAndFetch(ticksSpentScheduled);

        // If we're still pending, let the controller know and go back around for another go
        //
        // Otherwise we can think about exiting if the last call to run_for() wasn't very
        // productive
        if (stillPending) {
            _threadsPending.subtractAndFetch(1);
            stillPending = false;
        } else if (_threadsRunning.load() > _config->reservedThreads()) {
            // If we executed NO tasks, than just exit.
            if (tasksExecuted == 0) {
                log() << "Thread was idle for the past " << runTime << ". Exiting thread.";
                break;
            }

            // If we spent less than our idle threshold actually running tasks then exit the thread.
            // This time measurement doesn't include time spent running network callbacks, so the
            // threshold is lower than you'd expect.
            dassert(ticksSpentExecuting < spentRunning);
            dassert(spentRunning < std::numeric_limits<double>::max());

            // First get the ratio of ticks spent executing to ticks spent running. We expect this
            // to be <= 1.0
            double executingToRunning = ticksSpentExecuting / static_cast<double>(spentRunning);

            // Multiply that by 100 to get the percentage of time spent executing tasks. We expect
            // this to be <= 100.
            executingToRunning *= 100;
            dassert(executingToRunning <= 100);

            int pctExecuting = static_cast<int>(executingToRunning);
            if (pctExecuting < _config->idlePctThreshold()) {
                log() << "Thread was only executing tasks " << pctExecuting << "% over the last "
                      << runTime << ". Exiting thread.";
                break;
            }
        }
    }
}

void ServiceExecutorAdaptive::appendStats(BSONObjBuilder* bob) const {
    BSONObjBuilder section(bob->subobjStart("serviceExecutorTaskStats"));
    section << kExecutorLabel << kExecutorName  //
            << kTotalQueued << _totalQueued.load() << kTotalExecuted << _totalExecuted.load()
            << kTasksQueued << _tasksQueued.load() << kTasksExecuting << _tasksExecuting.load()
            << kTotalTimeRunningUs << ticksToMicros(_totalSpentRunning.load(), _tickSource)
            << kTotalTimeExecutingUs << ticksToMicros(_totalSpentExecuting.load(), _tickSource)
            << kTotalTimeQueuedUs << ticksToMicros(_totalSpentScheduled.load(), _tickSource)
            << kThreadsRunning << _threadsRunning.load() << kThreadsPending
            << _threadsPending.load();
    section.doneFast();
}

}  // namespace transport
}  // namespace mongo
