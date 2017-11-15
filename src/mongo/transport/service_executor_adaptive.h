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

#pragma once

#include <vector>

#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/list.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_executor.h"
#include "mongo/util/tick_source.h"

#include <asio.hpp>

namespace mongo {
namespace transport {

/**
 * This is an ASIO-based adaptive ServiceExecutor. It guarantees that threads will not become stuck
 * or deadlocked longer that its configured timeout and that idle threads will terminate themselves
 * if they spend more than its configure idle threshold idle.
 */
class ServiceExecutorAdaptive : public ServiceExecutor {
public:
    struct Options {
        virtual ~Options() = default;
        // The minimum number of threads the executor will keep running to service tasks.
        virtual int reservedThreads() const = 0;

        // The amount of time each worker thread runs before considering exiting because of
        // idleness.
        virtual Milliseconds workerThreadRunTime() const = 0;

        // workerThreadRuntime() is offset by a random value between -jitter and +jitter to prevent
        // thundering herds
        virtual int runTimeJitter() const = 0;

        // The amount of time the controller thread will wait before checking for stuck threads
        // to guarantee forward progress
        virtual Milliseconds stuckThreadTimeout() const = 0;

        // The maximum allowed latency between when a task is scheduled and a thread is started to
        // service it.
        virtual Microseconds maxQueueLatency() const = 0;

        // Threads that spend less than this threshold doing work during their workerThreadRunTime
        // period will exit
        virtual int idlePctThreshold() const = 0;

        // The maximum allowable depth of recursion for tasks scheduled with the MayRecurse flag
        // before stack unwinding is forced.
        virtual int recursionLimit() const = 0;
    };

    explicit ServiceExecutorAdaptive(ServiceContext* ctx, std::shared_ptr<asio::io_context> ioCtx);
    explicit ServiceExecutorAdaptive(ServiceContext* ctx,
                                     std::shared_ptr<asio::io_context> ioCtx,
                                     std::unique_ptr<Options> config);

    ServiceExecutorAdaptive(ServiceExecutorAdaptive&&) = default;
    ServiceExecutorAdaptive& operator=(ServiceExecutorAdaptive&&) = default;
    virtual ~ServiceExecutorAdaptive();

    Status start() final;
    Status shutdown(Milliseconds timeout) final;
    Status schedule(Task task, ScheduleFlags flags) final;

    Mode transportMode() const final {
        return Mode::kAsynchronous;
    }

    void appendStats(BSONObjBuilder* bob) const final;

    int threadsRunning() {
        return _threadsRunning.load();
    }

private:
    class TickTimer {
    public:
        explicit TickTimer(TickSource* tickSource)
            : _tickSource(tickSource),
              _ticksPerMillisecond(_tickSource->getTicksPerSecond() / 1000),
              _start(_tickSource->getTicks()) {
            invariant(_ticksPerMillisecond > 0);
        }

        TickSource::Tick sinceStartTicks() const {
            return _tickSource->getTicks() - _start.load();
        }

        Milliseconds sinceStart() const {
            return Milliseconds{sinceStartTicks() / _ticksPerMillisecond};
        }

        void reset() {
            _start.store(_tickSource->getTicks());
        }

    private:
        TickSource* const _tickSource;
        const TickSource::Tick _ticksPerMillisecond;
        AtomicWord<TickSource::Tick> _start;
    };

    class CumulativeTickTimer {
    public:
        CumulativeTickTimer(TickSource* ts) : _timer(ts) {}

        TickSource::Tick markStopped() {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            invariant(_running);
            _running = false;
            auto curTime = _timer.sinceStartTicks();
            _accumulator += curTime;
            return curTime;
        }

        void markRunning() {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            invariant(!_running);
            _timer.reset();
            _running = true;
        }

        TickSource::Tick totalTime() const {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            if (!_running)
                return _accumulator;
            return _timer.sinceStartTicks() + _accumulator;
        }

    private:
        TickTimer _timer;
        mutable stdx::mutex _mutex;
        TickSource::Tick _accumulator = 0;
        bool _running = false;
    };

    struct ThreadState {
        ThreadState(TickSource* ts) : running(ts), executing(ts) {}

        CumulativeTickTimer running;
        TickSource::Tick executingCurRun;
        CumulativeTickTimer executing;
        int recursionDepth = 0;
    };

    using ThreadList = stdx::list<ThreadState>;

    void _startWorkerThread();
    void _workerThreadRoutine(int threadId, ThreadList::iterator it);
    void _controllerThreadRoutine();
    bool _isStarved() const;
    Milliseconds _getThreadJitter() const;

    enum class ThreadTimer { Running, Executing };
    TickSource::Tick _getThreadTimerTotal(ThreadTimer which) const;

    std::shared_ptr<asio::io_context> _ioContext;

    std::unique_ptr<Options> _config;

    mutable stdx::mutex _threadsMutex;
    ThreadList _threads;
    stdx::thread _controllerThread;

    TickSource* const _tickSource;
    AtomicWord<bool> _isRunning{false};

    // These counters are used to detect stuck threads and high task queuing.
    AtomicWord<int> _threadsRunning{0};
    AtomicWord<int> _threadsPending{0};
    AtomicWord<int> _threadsInUse{0};
    AtomicWord<int> _tasksQueued{0};
    AtomicWord<int> _deferredTasksQueued{0};
    TickTimer _lastScheduleTimer;
    AtomicWord<TickSource::Tick> _pastThreadsSpentExecuting{0};
    AtomicWord<TickSource::Tick> _pastThreadsSpentRunning{0};
    static thread_local ThreadState* _localThreadState;

    // These counters are only used for reporting in serverStatus.
    AtomicWord<int64_t> _totalQueued{0};
    AtomicWord<int64_t> _totalExecuted{0};
    AtomicWord<TickSource::Tick> _totalSpentQueued{0};

    // Threads signal this condition variable when they exit so we can gracefully shutdown
    // the executor.
    stdx::condition_variable _deathCondition;

    // Tasks should signal this condition variable if they want the thread controller to
    // track their progress and do fast stuck detection
    stdx::condition_variable _scheduleCondition;
};

}  // namespace transport
}  // namespace mongo
