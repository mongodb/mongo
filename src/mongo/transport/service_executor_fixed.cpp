/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/transport/service_executor_fixed.h"

#include "mongo/base/error_codes.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/service_executor_gen.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/thread_safety_context.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor


namespace mongo::transport {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeSchedulingServiceExecutorFixedTask);
MONGO_FAIL_POINT_DEFINE(hangAfterServiceExecutorFixedExecutorThreadsStart);
MONGO_FAIL_POINT_DEFINE(hangBeforeServiceExecutorFixedLastExecutorThreadReturns);

Status inShutdownStatus() {
    return Status(ErrorCodes::ServiceExecutorInShutdown, "ServiceExecutorFixed is not running");
}

class Handle {
public:
    explicit Handle(std::shared_ptr<ServiceExecutorFixed> ptr) : _ptr{std::move(ptr)} {}

    ~Handle() {
        static constexpr Milliseconds timeout{Seconds{10}};
        while (!_ptr->shutdown(timeout).isOK()) {
            BSONObjBuilder stats;
            _ptr->appendStats(&stats);
            LOGV2(5744500,
                  "ServiceExecutorFixed::shutdown timed out. Retrying.",
                  "timeout"_attr = timeout,
                  "stats"_attr = stats.done());
        }
    }

    ServiceExecutorFixed* ptr() const {
        return _ptr.get();
    }

private:
    std::shared_ptr<ServiceExecutorFixed> _ptr;
};
const auto getHandle = ServiceContext::declareDecoration<std::unique_ptr<Handle>>();

const auto serviceExecutorFixedRegisterer = ServiceContext::ConstructorActionRegisterer{
    "ServiceExecutorFixed", [](ServiceContext* ctx) {
        getHandle(ctx) = std::make_unique<Handle>(std::make_shared<ServiceExecutorFixed>(
            ctx, ThreadPool::Limits{0, static_cast<size_t>(fixedServiceExecutorThreadLimit)}));
    }};
}  // namespace

struct ServiceExecutorFixed::Stats {
    size_t threadsRunning() const {
        auto ended = threadsEnded.load();
        auto started = threadsStarted.loadRelaxed();
        return started - ended;
    }

    size_t tasksRunning() const {
        auto ended = tasksEnded.load();
        auto started = tasksStarted.loadRelaxed();
        return started - ended;
    }

    size_t tasksLeft() const {
        auto ended = tasksEnded.load();
        auto scheduled = tasksScheduled.loadRelaxed();
        return scheduled - ended;
    }

    size_t tasksWaiting() const {
        auto ended = waitersEnded.load();
        auto started = waitersStarted.loadRelaxed();
        return started - ended;
    }

    size_t tasksTotal() const {
        return tasksRunning() + tasksWaiting();
    }

    AtomicWord<size_t> threadsStarted{0};
    AtomicWord<size_t> threadsEnded{0};

    AtomicWord<size_t> tasksScheduled{0};
    AtomicWord<size_t> tasksStarted{0};
    AtomicWord<size_t> tasksEnded{0};

    AtomicWord<size_t> waitersStarted{0};
    AtomicWord<size_t> waitersEnded{0};
};

class ServiceExecutorFixed::ExecutorThreadContext {
public:
    ExecutorThreadContext(ServiceExecutorFixed* serviceExecutor);
    ~ExecutorThreadContext();

    ExecutorThreadContext(ExecutorThreadContext&&) = delete;
    ExecutorThreadContext(const ExecutorThreadContext&) = delete;

    template <typename Task>
    void run(Task&& task) {
        // Yield here to improve concurrency, especially when there are more executor threads
        // than CPU cores.
        stdx::this_thread::yield();
        _executor->_stats->tasksStarted.fetchAndAdd(1);
        _recursionDepth++;

        ON_BLOCK_EXIT([&] {
            _recursionDepth--;
            _executor->_stats->tasksEnded.fetchAndAdd(1);

            auto lk = stdx::lock_guard(_executor->_mutex);
            _executor->_checkForShutdown();
        });

        std::forward<Task>(task)();
    }

    int getRecursionDepth() const {
        return _recursionDepth;
    }

private:
    ServiceExecutorFixed* const _executor;
    int _recursionDepth = 0;
};

ServiceExecutorFixed::ExecutorThreadContext::ExecutorThreadContext(
    ServiceExecutorFixed* serviceExecutor)
    : _executor(serviceExecutor) {
    _executor->_stats->threadsStarted.fetchAndAdd(1);
    hangAfterServiceExecutorFixedExecutorThreadsStart.pauseWhileSet();
}

ServiceExecutorFixed::ExecutorThreadContext::~ExecutorThreadContext() {
    auto ended = _executor->_stats->threadsEnded.addAndFetch(1);
    auto started = _executor->_stats->threadsStarted.loadRelaxed();
    if (ended == started) {
        hangBeforeServiceExecutorFixedLastExecutorThreadReturns.pauseWhileSet();
    }
}

thread_local std::unique_ptr<ServiceExecutorFixed::ExecutorThreadContext>
    ServiceExecutorFixed::_executorContext;

ServiceExecutorFixed::ServiceExecutorFixed(ServiceContext* ctx, ThreadPool::Limits limits)
    : _stats{std::make_unique<Stats>()},
      _svcCtx{ctx},
      _options{[&] {
          ThreadPool::Options opt(std::move(limits));
          opt.poolName = "ServiceExecutorFixed";
          opt.onCreateThread = [this](const auto&) {
              _executorContext = std::make_unique<ExecutorThreadContext>(this);
          };
          return opt;
      }()},
      _threadPool{std::make_shared<ThreadPool>(_options)} {}

ServiceExecutorFixed::~ServiceExecutorFixed() {
    _finalize();
}

void ServiceExecutorFixed::_finalize() noexcept {
    LOGV2_DEBUG(4910502,
                kDiagnosticLogLevel,
                "Joining fixed thread-pool service executor",
                "name"_attr = _options.poolName);

    if (std::shared_ptr<ThreadPool> pool = [&] {
            auto lk = stdx::unique_lock(_mutex);
            _beginShutdown();
            _waitForStop(lk, {});
            return std::exchange(_threadPool, nullptr);
        }()) {
        pool->shutdown();
        pool->join();
    }

    invariant(_stats->threadsRunning() == 0);
    invariant(_stats->tasksRunning() == 0);
    invariant(_stats->tasksWaiting() == 0);
}

Status ServiceExecutorFixed::start() {
    {
        auto lk = stdx::lock_guard(_mutex);
        switch (_state) {
            case State::kNotStarted:
                _state = State::kRunning;
                break;
            case State::kRunning:
                return Status::OK();
            case State::kStopping:
            case State::kStopped:
                return {ErrorCodes::ServiceExecutorInShutdown,
                        "ServiceExecutorFixed is already stopping or stopped"};
        }
    }

    LOGV2_DEBUG(4910501,
                kDiagnosticLogLevel,
                "Starting fixed thread-pool service executor",
                "name"_attr = _options.poolName);

    _threadPool->startup();

    if (!_svcCtx) {
        // For some tests, we do not have a ServiceContext.
        invariant(TestingProctor::instance().isEnabled());
        return Status::OK();
    }

    auto tl = _svcCtx->getTransportLayer();
    if (!tl) {
        // For some tests, we do not have a TransportLayer.
        invariant(TestingProctor::instance().isEnabled());
        return Status::OK();
    }

    auto reactor = tl->getReactor(TransportLayer::WhichReactor::kIngress);
    invariant(reactor);
    _threadPool->schedule([this, reactor](Status) {
        {
            // Check to make sure we haven't been shutdown already. Note that there is still a brief
            // race that immediately follows this check. ASIOReactor::stop() is not permanent, thus
            // our run() could "restart" the reactor.
            auto lk = stdx::lock_guard(_mutex);
            if (_state != State::kRunning) {
                return;
            }
        }

        // Start running on the reactor immediately.
        reactor->run();
    });

    return Status::OK();
}

ServiceExecutorFixed* ServiceExecutorFixed::get(ServiceContext* ctx) {
    auto&& handle = getHandle(ctx);
    invariant(handle);
    return handle->ptr();
}

bool ServiceExecutorFixed::_waitForStop(stdx::unique_lock<Mutex>& lk,
                                        boost::optional<Milliseconds> timeout) {
    auto isStopped = [&] { return _state == State::kStopped; };
    if (timeout)
        return _shutdownCondition.wait_for(lk, timeout->toSystemDuration(), isStopped);
    _shutdownCondition.wait(lk, isStopped);
    return true;
}

Status ServiceExecutorFixed::shutdown(Milliseconds timeout) {
    LOGV2_DEBUG(4910503,
                kDiagnosticLogLevel,
                "Shutting down fixed thread-pool service executor",
                "name"_attr = _name());

    {
        auto lk = stdx::unique_lock(_mutex);
        _beginShutdown();

        // There is a world where we are able to simply do a timed wait upon a future chain.
        // However, that world likely requires an OperationContext available through shutdown.
        if (!_waitForStop(lk, timeout)) {
            return Status(ErrorCodes::ExceededTimeLimit,
                          "Failed to shutdown all executor threads within the time limit");
        }
    }

    _finalize();
    LOGV2_DEBUG(4910504,
                kDiagnosticLogLevel,
                "Shutdown fixed thread-pool service executor",
                "name"_attr = _name());

    return Status::OK();
}

void ServiceExecutorFixed::_beginShutdown() {
    switch (_state) {
        case State::kNotStarted:
            invariant(_waiters.empty());
            invariant(_stats->tasksLeft() == 0);
            _state = State::kStopped;
            break;
        case State::kRunning:
            _state = State::kStopping;
            // Cancel any session we own.
            for (auto& waiter : _waiters)
                waiter.session->cancelAsyncOperations();
            // There may not be outstanding threads, check for shutdown now.
            _checkForShutdown();
            break;
        case State::kStopping:
            break;  // Just nead to wait it out.
        case State::kStopped:
            break;
    }
}

const std::string& ServiceExecutorFixed::_name() const {
    return _options.poolName;
}

void ServiceExecutorFixed::_checkForShutdown() {
    if (_state == State::kRunning)
        return;  // We're actively running.
    if (!_waiters.empty())
        return;  // We still have some in wait.
    if (_stats->tasksLeft() > 0)
        return;

    // We have achieved a soft form of shutdown:
    // - _state != kRunning means that there will be no new external tasks or waiters.
    // - _waiters.empty() means that all network waits have finished and there will be no new
    //   internal tasks.
    // - _tasksLeft() == 0 means that all tasks, both internal and external have finished.
    //
    // From this point on, all of our threads will be idle.
    // When the dtor runs, the thread pool will perform a trivial shutdown() and join().
    _state = State::kStopped;

    LOGV2_DEBUG(4910505, kDiagnosticLogLevel, "Finishing shutdown", "name"_attr = _name());
    _shutdownCondition.notify_one();

    if (!_svcCtx) {
        // For some tests, we do not have a ServiceContext.
        invariant(TestingProctor::instance().isEnabled());
        return;
    }

    auto tl = _svcCtx->getTransportLayer();
    if (!tl) {
        // For some tests, we do not have a TransportLayer.
        invariant(TestingProctor::instance().isEnabled());
        return;
    }

    auto reactor = tl->getReactor(TransportLayer::WhichReactor::kIngress);
    invariant(reactor);
    reactor->stop();
}

void ServiceExecutorFixed::_schedule(Task task) {
    {
        auto lk = stdx::unique_lock(_mutex);
        if (_state != State::kRunning) {
            lk.unlock();
            task(inShutdownStatus());
            return;
        }

        _stats->tasksScheduled.fetchAndAdd(1);
    }

    hangBeforeSchedulingServiceExecutorFixedTask.pauseWhileSet();
    _threadPool->schedule([this, task = std::move(task)](Status status) mutable {
        _executorContext->run([&] { task(std::move(status)); });
    });
}

size_t ServiceExecutorFixed::getRunningThreads() const {
    return _stats->threadsRunning();
}

void ServiceExecutorFixed::_runOnDataAvailable(const SessionHandle& session,
                                               Task onCompletionCallback) {
    invariant(session);
    yieldIfAppropriate();

    // Make sure we're still allowed to schedule and track the session
    auto lk = stdx::unique_lock(_mutex);
    if (_state != State::kRunning) {
        lk.unlock();
        onCompletionCallback(inShutdownStatus());
        return;
    }

    auto it = _waiters.insert(_waiters.end(), {session, std::move(onCompletionCallback)});
    _stats->waitersStarted.fetchAndAdd(1);

    lk.unlock();

    auto anchor = shared_from_this();
    session->asyncWaitForData()
        .thenRunOn(makeTaskRunner())
        .getAsync([this, anchor, it](Status status) {
            // Remove our waiter from the list.
            auto lk = stdx::unique_lock(_mutex);
            auto waiter = std::exchange(*it, {});
            _waiters.erase(it);
            _stats->waitersEnded.fetchAndAdd(1);
            lk.unlock();

            waiter.session = nullptr;
            waiter.onCompletionCallback(std::move(status));
        });
}

void ServiceExecutorFixed::appendStats(BSONObjBuilder* bob) const {
    // The ServiceExecutorFixed schedules Clients temporarily onto its threads and waits
    // asynchronously.
    BSONObjBuilder subbob = bob->subobjStart("fixed");
    subbob.append("threadsRunning", static_cast<int>(_stats->threadsRunning()));
    subbob.append("clientsInTotal", static_cast<int>(_stats->tasksTotal()));
    subbob.append("clientsRunning", static_cast<int>(_stats->tasksRunning()));
    subbob.append("clientsWaitingForData", static_cast<int>(_stats->tasksWaiting()));
}

int ServiceExecutorFixed::getRecursionDepthForExecutorThread() const {
    invariant(_executorContext);
    return _executorContext->getRecursionDepth();
}

auto ServiceExecutorFixed::makeTaskRunner() -> std::unique_ptr<TaskRunner> {
    iassert(ErrorCodes::ShutdownInProgress, "Executor is not running", _state == State::kRunning);

    /** Schedules on this. */
    class ForwardingTaskRunner : public TaskRunner {
    public:
        explicit ForwardingTaskRunner(ServiceExecutorFixed* e) : _e{e} {}

        void schedule(Task task) override {
            _e->_schedule(std::move(task));
        }

        void runOnDataAvailable(std::shared_ptr<Session> session, Task task) override {
            _e->_runOnDataAvailable(std::move(session), std::move(task));
        }

    private:
        ServiceExecutorFixed* _e;
    };
    return std::make_unique<ForwardingTaskRunner>(this);
}

}  // namespace mongo::transport
