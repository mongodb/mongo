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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor

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

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeSchedulingServiceExecutorFixedTask);
MONGO_FAIL_POINT_DEFINE(hangAfterServiceExecutorFixedExecutorThreadsStart);
MONGO_FAIL_POINT_DEFINE(hangBeforeServiceExecutorFixedLastExecutorThreadReturns);

namespace transport {
namespace {
constexpr auto kThreadsRunning = "threadsRunning"_sd;
constexpr auto kExecutorLabel = "executor"_sd;
constexpr auto kExecutorName = "fixed"_sd;

const auto getServiceExecutorFixed =
    ServiceContext::declareDecoration<std::shared_ptr<ServiceExecutorFixed>>();

const auto serviceExecutorFixedRegisterer = ServiceContext::ConstructorActionRegisterer{
    "ServiceExecutorFixed", [](ServiceContext* ctx) {
        auto limits = ThreadPool::Limits{};
        limits.minThreads = 0;
        limits.maxThreads = fixedServiceExecutorThreadLimit;
        getServiceExecutorFixed(ctx) =
            std::make_shared<ServiceExecutorFixed>(ctx, std::move(limits));
    }};
}  // namespace

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
        _executor->_stats.tasksStarted.fetchAndAdd(1);
        _recursionDepth++;

        ON_BLOCK_EXIT([&] {
            _recursionDepth--;
            _executor->_stats.tasksEnded.fetchAndAdd(1);

            auto lk = stdx::lock_guard(_executor->_mutex);
            _executor->_checkForShutdown(lk);
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
    _executor->_stats.threadsStarted.fetchAndAdd(1);
    hangAfterServiceExecutorFixedExecutorThreadsStart.pauseWhileSet();
}

ServiceExecutorFixed::ExecutorThreadContext::~ExecutorThreadContext() {
    auto ended = _executor->_stats.threadsEnded.addAndFetch(1);
    auto started = _executor->_stats.threadsStarted.loadRelaxed();
    if (ended == started) {
        hangBeforeServiceExecutorFixedLastExecutorThreadReturns.pauseWhileSet();
    }
}

thread_local std::unique_ptr<ServiceExecutorFixed::ExecutorThreadContext>
    ServiceExecutorFixed::_executorContext;

ServiceExecutorFixed::ServiceExecutorFixed(ServiceContext* ctx, ThreadPool::Limits limits)
    : _svcCtx{ctx}, _options(std::move(limits)) {
    _options.poolName = "ServiceExecutorFixed";
    _options.onCreateThread = [this](const auto&) {
        _executorContext = std::make_unique<ExecutorThreadContext>(this);
    };

    _threadPool = std::make_shared<ThreadPool>(_options);
}

ServiceExecutorFixed::~ServiceExecutorFixed() {
    switch (_state) {
        case State::kNotStarted:
            return;
        case State::kRunning: {
            // We should not be running while in this destructor.
            MONGO_UNREACHABLE;
        }
        case State::kStopping:
        case State::kStopped: {
            // We can go ahead and attempt to join our thread pool.
        } break;
        default: { MONGO_UNREACHABLE; }
    }

    LOGV2_DEBUG(4910502,
                kDiagnosticLogLevel,
                "Shutting down pool for fixed thread-pool service executor",
                "name"_attr = _options.poolName);

    // We only can desturct when we have joined all of our tasks and canceled all of our sessions.
    // This thread pool doesn't get to refuse work over its lifetime. It's possible that tasks are
    // stiil blocking. If so, we block until they finish here.
    _threadPool->shutdown();
    _threadPool->join();

    invariant(_threadsRunning() == 0);
    invariant(_tasksRunning() == 0);
    invariant(_waiters.empty());
}

Status ServiceExecutorFixed::start() {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        switch (_state) {
            case State::kNotStarted: {
                // Time to start
                _state = State::kRunning;
            } break;
            case State::kRunning: {
                return Status::OK();
            }
            case State::kStopping:
            case State::kStopped: {
                return {ErrorCodes::ServiceExecutorInShutdown,
                        "ServiceExecutorFixed is already stopping or stopped"};
            }
            default: { MONGO_UNREACHABLE; }
        };
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
    invariant(tl);

    auto reactor = tl->getReactor(TransportLayer::WhichReactor::kIngress);
    invariant(reactor);
    _threadPool->schedule([this, reactor](Status) {
        {
            // Check to make sure we haven't been shutdown already. Note that there is still a brief
            // race that immediately follows this check. ASIOReactor::stop() is not permanent, thus
            // our run() could "restart" the reactor.
            stdx::lock_guard<Latch> lk(_mutex);
            if (_state != kRunning) {
                return;
            }
        }

        // Start running on the reactor immediately.
        reactor->run();
    });

    return Status::OK();
}

ServiceExecutorFixed* ServiceExecutorFixed::get(ServiceContext* ctx) {
    auto& ref = getServiceExecutorFixed(ctx);
    invariant(ref);
    return ref.get();
}

Status ServiceExecutorFixed::shutdown(Milliseconds timeout) {
    LOGV2_DEBUG(4910503,
                kDiagnosticLogLevel,
                "Shutting down fixed thread-pool service executor",
                "name"_attr = _options.poolName);

    {
        auto lk = stdx::unique_lock(_mutex);

        switch (_state) {
            case State::kNotStarted:
            case State::kRunning: {
                _state = State::kStopping;

                for (auto& waiter : _waiters) {
                    // Cancel any session we own.
                    waiter.session->cancelAsyncOperations();
                }

                // There may not be outstanding threads, check for shutdown now.
                _checkForShutdown(lk);

                if (_state == State::kStopped) {
                    // We were able to become stopped immediately.
                    return Status::OK();
                }
            } break;
            case State::kStopping: {
                // Just nead to wait it out.
            } break;
            case State::kStopped: {
                // Totally done.
                return Status::OK();
            } break;
            default: { MONGO_UNREACHABLE; }
        }
    }

    LOGV2_DEBUG(4910504,
                kDiagnosticLogLevel,
                "Waiting for shutdown of fixed thread-pool service executor",
                "name"_attr = _options.poolName);

    // There is a world where we are able to simply do a timed wait upon a future chain. However,
    // that world likely requires an OperationContext available through shutdown.
    auto lk = stdx::unique_lock(_mutex);
    if (!_shutdownCondition.wait_for(
            lk, timeout.toSystemDuration(), [this] { return _state == State::kStopped; })) {
        return Status(ErrorCodes::ExceededTimeLimit,
                      "Failed to shutdown all executor threads within the time limit");
    }

    return Status::OK();
}

void ServiceExecutorFixed::_checkForShutdown(WithLock) {
    if (_state == State::kRunning) {
        // We're actively running.
        return;
    }
    invariant(_state != State::kNotStarted);

    if (!_waiters.empty()) {
        // We still have some in wait.
        return;
    }

    auto tasksLeft = _tasksLeft();
    if (tasksLeft > 0) {
        // We have tasks remaining.
        return;
    }
    invariant(tasksLeft == 0);

    // We have achieved a soft form of shutdown:
    // - _state != kRunning means that there will be no new external tasks or waiters.
    // - _waiters.empty() means that all network waits have finished and there will be no new
    //   internal tasks.
    // - _tasksLeft() == 0 means that all tasks, both internal and external have finished.
    //
    // From this point on, all of our threads will be idle. When the dtor runs, the thread pool will
    // experience a trivial shutdown() and join().
    _state = State::kStopped;

    LOGV2_DEBUG(
        4910505, kDiagnosticLogLevel, "Finishing shutdown", "name"_attr = _options.poolName);
    _shutdownCondition.notify_one();

    if (!_svcCtx) {
        // For some tests, we do not have a ServiceContext.
        invariant(TestingProctor::instance().isEnabled());
        return;
    }

    auto tl = _svcCtx->getTransportLayer();
    invariant(tl);

    auto reactor = tl->getReactor(TransportLayer::WhichReactor::kIngress);
    invariant(reactor);
    reactor->stop();
}

Status ServiceExecutorFixed::scheduleTask(Task task, ScheduleFlags flags) {
    {
        auto lk = stdx::unique_lock(_mutex);
        if (_state != State::kRunning) {
            return kInShutdown;
        }

        _stats.tasksScheduled.fetchAndAdd(1);
    }

    auto mayExecuteTaskInline = [&] {
        if (!(flags & ScheduleFlags::kMayRecurse))
            return false;
        if (!_executorContext)
            return false;
        return true;
    };


    if (mayExecuteTaskInline()) {
        invariant(_executorContext);
        if (_executorContext->getRecursionDepth() <
            fixedServiceExecutorRecursionLimit.loadRelaxed()) {
            // Recursively executing the task on the executor thread.
            _executorContext->run(std::move(task));
            return Status::OK();
        }
    }

    hangBeforeSchedulingServiceExecutorFixedTask.pauseWhileSet();

    _threadPool->schedule([this, task = std::move(task)](Status status) mutable {
        invariant(status);

        _executorContext->run([&] { task(); });
    });

    return Status::OK();
}

void ServiceExecutorFixed::_schedule(OutOfLineExecutor::Task task) noexcept {
    {
        auto lk = stdx::unique_lock(_mutex);
        if (_state != State::kRunning) {
            lk.unlock();

            task(kInShutdown);
            return;
        }

        _stats.tasksScheduled.fetchAndAdd(1);
    }

    _threadPool->schedule([this, task = std::move(task)](Status status) mutable {
        _executorContext->run([&] { task(std::move(status)); });
    });
}

void ServiceExecutorFixed::runOnDataAvailable(const SessionHandle& session,
                                              OutOfLineExecutor::Task onCompletionCallback) {
    invariant(session);

    auto waiter = Waiter{session, std::move(onCompletionCallback)};

    WaiterList::iterator it;
    {
        // Make sure we're still allowed to schedule and track the session
        auto lk = stdx::unique_lock(_mutex);
        if (_state != State::kRunning) {
            lk.unlock();
            waiter.onCompletionCallback(kInShutdown);
            return;
        }

        it = _waiters.emplace(_waiters.end(), std::move(waiter));
    }

    session->asyncWaitForData()
        .thenRunOn(shared_from_this())
        .getAsync([this, anchor = shared_from_this(), it](Status status) mutable {
            Waiter waiter;
            {
                // Remove our waiter from the list.
                auto lk = stdx::unique_lock(_mutex);
                waiter = std::exchange(*it, {});
                _waiters.erase(it);
            }

            waiter.onCompletionCallback(std::move(status));
        });
}

void ServiceExecutorFixed::appendStats(BSONObjBuilder* bob) const {
    *bob << kExecutorLabel << kExecutorName << kThreadsRunning
         << static_cast<int>(_threadsRunning());
}

int ServiceExecutorFixed::getRecursionDepthForExecutorThread() const {
    invariant(_executorContext);
    return _executorContext->getRecursionDepth();
}

}  // namespace transport
}  // namespace mongo
