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

// IWYU pragma: no_include "cxxabi.h"
#include <deque>
#include <mutex>
#include <string>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/transport/service_executor_utils.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/functional.h"
#include "mongo/util/out_of_line_executor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor

namespace mongo::transport {

namespace service_executor_synchronous_detail {

class ServiceExecutorSyncImpl::SharedState : public std::enable_shared_from_this<SharedState> {
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
        stdx::unique_lock<stdx::mutex> _lk{_p->_mutex};
    };

public:
    explicit SharedState(RunTaskInline runTaskInline) : _runTaskInline(runTaskInline) {}

    void schedule(Task task, StringData name);

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

    const RunTaskInline _runTaskInline;
    mutable stdx::mutex _mutex;
    stdx::condition_variable _cv;
    AtomicWord<bool> _isRunning;
    size_t _threads = 0;
};

class ServiceExecutorSyncImpl::SharedState::WorkerThreadInfo {
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

void ServiceExecutorSyncImpl::SharedState::schedule(Task task, StringData name) {
    using namespace fmt::literals;
    if (!isRunning()) {
        task(Status(ErrorCodes::ShutdownInProgress, "{} is not running"_format(name)));
        return;
    }

    thread_local WorkerThreadInfo* workerThreadInfoTls = nullptr;

    if (workerThreadInfoTls) {
        workerThreadInfoTls->queue.push_back(std::move(task));
        return;
    }

    auto workerInfo = std::make_unique<WorkerThreadInfo>(shared_from_this());
    workerInfo->queue.push_back(std::move(task));

    auto runTask = [w = std::move(workerInfo)] {
        w->sharedState->lock().onStartThread();
        ScopeGuard onEndThreadGuard = [&] {
            w->sharedState->lock().onEndThread();
        };

        workerThreadInfoTls = &*w;
        ScopeGuard resetTlsGuard = [&] {
            workerThreadInfoTls = nullptr;
        };

        w->run();
    };

    if (_runTaskInline == RunTaskInline{true}) {
        runTask();
    } else {
        // The usual way to fail to schedule is to invoke the task,
        // but in this case we will not have the task anymore.
        // We will have given it away while attempting to launch the thread.
        LOGV2_DEBUG(22983, 3, "Starting ServiceExecutorSynchronous worker thread");
        iassert(launchServiceWorkerThread(std::move(runTask)));
    }
}

ServiceExecutorSyncImpl::ServiceExecutorSyncImpl(RunTaskInline runTaskInline,
                                                 std::string statsFieldName)
    : _sharedState{std::make_shared<SharedState>(runTaskInline)},
      _statsFieldName(std::move(statsFieldName)) {}

ServiceExecutorSyncImpl::~ServiceExecutorSyncImpl() = default;

void ServiceExecutorSyncImpl::start() {
    _sharedState->setIsRunning(true);
}

Status ServiceExecutorSyncImpl::shutdown(Milliseconds timeout) {
    LOGV2_DEBUG(22982, 3, "Shutting down passthrough executor");
    auto stopLock = _sharedState->lock();
    _sharedState->setIsRunning(false);
    if (!stopLock.waitForDrain(timeout))
        return Status(ErrorCodes::Error::ExceededTimeLimit,
                      "passthrough executor couldn't shutdown "
                      "all worker threads within time limit.");
    return Status::OK();
}

size_t ServiceExecutorSyncImpl::getRunningThreads() const {
    return _sharedState->lock().threads();
}

void ServiceExecutorSyncImpl::appendStats(BSONObjBuilder* bob) const {
    // Has one client per thread and waits synchronously on that thread.
    int threads = getRunningThreads();
    BSONObjBuilder{bob->subobjStart(_statsFieldName)}
        .append("threadsRunning", threads)
        .append("clientsInTotal", threads)
        .append("clientsRunning", threads)
        .append("clientsWaitingForData", 0);
}

auto ServiceExecutorSyncImpl::makeTaskRunner() -> std::unique_ptr<TaskRunner> {
    if (!_sharedState->isRunning())
        iassert(Status(ErrorCodes::ShutdownInProgress, "Executor is not running"));

    /** Schedules on this. */
    class ForwardingTaskRunner : public TaskRunner {
    public:
        explicit ForwardingTaskRunner(ServiceExecutorSyncImpl* e) : _e{e} {}

        void schedule(Task task) override {
            _e->_sharedState->schedule(std::move(task), _e->getName());
        }

        void runOnDataAvailable(std::shared_ptr<Session> session, Task task) override {
            invariant(session);
            _e->yieldIfAppropriate();
            _e->_sharedState->schedule(std::move(task), _e->getName());
        }

    private:
        ServiceExecutorSyncImpl* _e;
    };
    return std::make_unique<ForwardingTaskRunner>(this);
}

}  // namespace service_executor_synchronous_detail

/////////////////////////////
// ServiceExecutorSynchronous
/////////////////////////////

namespace {
const auto getServiceExecutorSynchronous =
    ServiceContext::declareDecoration<std::unique_ptr<ServiceExecutorSynchronous>>();

const ServiceContext::ConstructorActionRegisterer serviceExecutorSynchronousRegisterer{
    "ServiceExecutorSynchronous", [](ServiceContext* ctx) {
        getServiceExecutorSynchronous(ctx) = std::make_unique<ServiceExecutorSynchronous>();
    }};
}  // namespace

ServiceExecutorSynchronous* ServiceExecutorSynchronous::get(ServiceContext* ctx) {
    auto& ref = getServiceExecutorSynchronous(ctx);
    invariant(ref);
    return ref.get();
}

/////////////////////////
// Service ExecutorInline
/////////////////////////

namespace {
const auto getServiceExecutorInline =
    ServiceContext::declareDecoration<std::unique_ptr<ServiceExecutorInline>>();

const ServiceContext::ConstructorActionRegisterer serviceExecutorInlineRegisterer{
    "ServiceExecutorInline", [](ServiceContext* ctx) {
        getServiceExecutorInline(ctx) = std::make_unique<ServiceExecutorInline>();
    }};
}  // namespace

ServiceExecutorInline* ServiceExecutorInline::get(ServiceContext* ctx) {
    auto& ref = getServiceExecutorInline(ctx);
    invariant(ref);
    return ref.get();
}

}  // namespace mongo::transport
