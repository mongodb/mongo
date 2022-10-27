/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/transport/service_executor_synchronous.h"

#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_executor_utils.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor

namespace mongo::transport {
namespace {
const auto getServiceExecutorSynchronous =
    ServiceContext::declareDecoration<std::unique_ptr<ServiceExecutorSynchronous>>();

const ServiceContext::ConstructorActionRegisterer serviceExecutorSynchronousRegisterer{
    "ServiceExecutorSynchronous", [](ServiceContext* ctx) {
        getServiceExecutorSynchronous(ctx) = std::make_unique<ServiceExecutorSynchronous>(ctx);
    }};
}  // namespace

class ServiceExecutorSynchronous::SharedState : public std::enable_shared_from_this<SharedState> {
private:
    class LockRef {
    public:
        explicit LockRef(SharedState* p) : _p{p} {}

        size_t threads() const {
            return _p->_threads;
        }

        bool waitForDrain(Milliseconds dur) {
            return _p->_cv.wait_for(_lk, dur.toSystemDuration(), [&] { return !_p->_threads; });
        }

        void onStartThread() {
            ++_p->_threads;
        }

        void onEndThread() {
            if (!--_p->_threads)
                _p->_cv.notify_all();
        }

    private:
        SharedState* _p;
        stdx::unique_lock<stdx::mutex> _lk{_p->_mutex};  // NOLINT
    };

public:
    void schedule(Task task);

    bool isRunning() const {
        return _isRunning.loadRelaxed();
    }

    void setIsRunning(bool b) {
        _isRunning.store(b);
    }

    LockRef lock() {
        return LockRef{this};
    }

private:
    class WorkerThreadInfo;

    mutable stdx::mutex _mutex;  // NOLINT
    stdx::condition_variable _cv;
    AtomicWord<bool> _isRunning;
    size_t _threads = 0;
};

class ServiceExecutorSynchronous::SharedState::WorkerThreadInfo {
public:
    explicit WorkerThreadInfo(std::shared_ptr<SharedState> sharedState)
        : sharedState{std::move(sharedState)} {}

    void run() {
        while (!queue.empty() && sharedState->isRunning()) {
            queue.front()(Status::OK());
            queue.pop_front();
        }
    }

    std::shared_ptr<SharedState> sharedState;
    std::deque<Task> queue;
};

void ServiceExecutorSynchronous::SharedState::schedule(Task task) {
    if (!isRunning()) {
        task(Status(ErrorCodes::ShutdownInProgress, "Executor is not running"));
        return;
    }

    thread_local WorkerThreadInfo* workerThreadInfoTls = nullptr;

    if (workerThreadInfoTls) {
        workerThreadInfoTls->queue.push_back(std::move(task));
        return;
    }

    LOGV2_DEBUG(22983, 3, "Starting ServiceExecutorSynchronous worker thread");
    auto workerInfo = std::make_unique<WorkerThreadInfo>(shared_from_this());
    workerInfo->queue.push_back(std::move(task));

    Status status = launchServiceWorkerThread([w = std::move(workerInfo)] {
        w->sharedState->lock().onStartThread();
        ScopeGuard onEndThreadGuard = [&] { w->sharedState->lock().onEndThread(); };

        workerThreadInfoTls = &*w;
        ScopeGuard resetTlsGuard = [&] { workerThreadInfoTls = nullptr; };

        w->run();
    });
    // The usual way to fail to schedule is to invoke the task, but in this case
    // we don't have the task anymore. We gave it away to the callback that the
    // failed thread was supposed to run.
    iassert(status);
}

ServiceExecutorSynchronous::ServiceExecutorSynchronous(ServiceContext*)
    : _sharedState{std::make_shared<SharedState>()} {}

ServiceExecutorSynchronous::~ServiceExecutorSynchronous() = default;

Status ServiceExecutorSynchronous::start() {
    _sharedState->setIsRunning(true);
    return Status::OK();
}

Status ServiceExecutorSynchronous::shutdown(Milliseconds timeout) {
    LOGV2_DEBUG(22982, 3, "Shutting down passthrough executor");
    auto stopLock = _sharedState->lock();
    _sharedState->setIsRunning(false);
    if (!stopLock.waitForDrain(timeout))
        return Status(ErrorCodes::Error::ExceededTimeLimit,
                      "passthrough executor couldn't shutdown "
                      "all worker threads within time limit.");
    return Status::OK();
}

ServiceExecutorSynchronous* ServiceExecutorSynchronous::get(ServiceContext* ctx) {
    auto& ref = getServiceExecutorSynchronous(ctx);
    invariant(ref);
    return ref.get();
}

void ServiceExecutorSynchronous::_schedule(Task task) {
    _sharedState->schedule(std::move(task));
}

size_t ServiceExecutorSynchronous::getRunningThreads() const {
    return _sharedState->lock().threads();
}

void ServiceExecutorSynchronous::appendStats(BSONObjBuilder* bob) const {
    // Has one client per thread and waits synchronously on that thread.
    int threads = getRunningThreads();
    BSONObjBuilder{bob->subobjStart("passthrough")}
        .append("threadsRunning", threads)
        .append("clientsInTotal", threads)
        .append("clientsRunning", threads)
        .append("clientsWaitingForData", 0);
}

void ServiceExecutorSynchronous::_runOnDataAvailable(const SessionHandle& session, Task task) {
    invariant(session);
    yieldIfAppropriate();
    _schedule(std::move(task));
}

auto ServiceExecutorSynchronous::makeTaskRunner() -> std::unique_ptr<TaskRunner> {
    if (!_sharedState->isRunning())
        iassert(Status(ErrorCodes::ShutdownInProgress, "Executor is not running"));

    /** Schedules on this. */
    class ForwardingTaskRunner : public TaskRunner {
    public:
        explicit ForwardingTaskRunner(ServiceExecutorSynchronous* e) : _e{e} {}

        void schedule(Task task) override {
            _e->_schedule(std::move(task));
        }

        void runOnDataAvailable(std::shared_ptr<Session> session, Task task) override {
            _e->_runOnDataAvailable(std::move(session), std::move(task));
        }

    private:
        ServiceExecutorSynchronous* _e;
    };
    return std::make_unique<ForwardingTaskRunner>(this);
}

}  // namespace mongo::transport
