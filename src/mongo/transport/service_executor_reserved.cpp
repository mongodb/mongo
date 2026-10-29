// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


// IWYU pragma: no_include "cxxabi.h"
#include "mongo/transport/service_executor_reserved.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/service_executor_utils.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/functional.h"
#include "mongo/util/out_of_line_executor.h"

#include <algorithm>
#include <mutex>
#include <string_view>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor


namespace mongo {
namespace transport {
namespace {
using namespace std::literals::string_view_literals;

constexpr auto kExecutorName = "reserved"sv;

constexpr auto kThreadsRunning = "threadsRunning"sv;
constexpr auto kClientsInTotal = "clientsInTotal"sv;
constexpr auto kClientsRunning = "clientsRunning"sv;
constexpr auto kClientsWaiting = "clientsWaitingForData"sv;

const auto getServiceExecutorReserved =
    ServiceContext::declareDecoration<std::unique_ptr<ServiceExecutorReserved>>();

const auto serviceExecutorReservedRegisterer = ServiceContext::ConstructorActionRegisterer{
    "ServiceExecutorReserved", [](ServiceContext* ctx) {
        if (!serverGlobalParams.reservedAdminThreads) {
            return;
        }

        getServiceExecutorReserved(ctx) = std::make_unique<transport::ServiceExecutorReserved>(
            ctx, "admin/internal connections", serverGlobalParams.reservedAdminThreads);
    }};
}  // namespace

thread_local std::deque<ServiceExecutor::Task> ServiceExecutorReserved::_localWorkQueue = {};
thread_local int64_t ServiceExecutorReserved::_localThreadIdleCounter = 0;

ServiceExecutorReserved::ServiceExecutorReserved(ServiceContext* ctx,
                                                 std::string name,
                                                 size_t reservedThreads)
    : _name(std::move(name)), _reservedThreads(reservedThreads) {}

void ServiceExecutorReserved::start() {
    {
        std::unique_lock<std::mutex> lk(_mutex);
        _stillRunning.store(true);
        _numStartingThreads.store(_reservedThreads);
    }

    for (size_t i = 0; i < _reservedThreads; i++) {
        uassertStatusOK(_startWorker());
    }
}

Status ServiceExecutorReserved::_startWorker() {
    LOGV2(22978, "Starting new worker thread for service executor", "name"_attr = _name);
    return launchServiceWorkerThread([this] {
        std::unique_lock<std::mutex> lk(_mutex);
        _numRunningWorkerThreads.addAndFetch(1);
        ScopeGuard numRunningGuard([&] {
            _numRunningWorkerThreads.subtractAndFetch(1);
            _shutdownCondition.notify_one();
        });

        _numStartingThreads.subtractAndFetch(1);
        _numReadyThreads.addAndFetch(1);

        while (_stillRunning.load()) {
            _threadWakeup.wait(lk, [&] { return (!_stillRunning.load() || !_readyTasks.empty()); });

            if (!_stillRunning.loadRelaxed()) {
                break;
            }

            if (_readyTasks.empty()) {
                continue;
            }

            auto task = std::move(_readyTasks.front());
            _readyTasks.pop_front();
            _numReadyThreads.subtractAndFetch(1);
            bool launchReplacement = false;
            // lk is still needed here to prevent a toctou bug.
            if (_numReadyThreads.load() + _numStartingThreads.load() < _reservedThreads) {
                _numStartingThreads.addAndFetch(1);
                launchReplacement = true;
            }

            lk.unlock();

            if (launchReplacement) {
                auto threadStartStatus = _startWorker();
                if (!threadStartStatus.isOK()) {
                    LOGV2_WARNING(22981,
                                  "Could not start new reserve worker thread",
                                  "error"_attr = threadStartStatus);
                    _numStartingThreads.subtractAndFetch(1);
                }
            }

            _localWorkQueue.emplace_back(std::move(task));
            while (!_localWorkQueue.empty() && _stillRunning.loadRelaxed()) {
                _localWorkQueue.front()(Status::OK());
                _localWorkQueue.pop_front();
            }

            lk.lock();
            if (_numReadyThreads.load() + 1 > _reservedThreads) {
                break;
            } else {
                _numReadyThreads.addAndFetch(1);
            }
        }

        LOGV2_DEBUG(22979, 3, "Exiting worker thread in service executor", "name"_attr = _name);
    });
}

ServiceExecutorReserved* ServiceExecutorReserved::get(ServiceContext* ctx) {
    auto& ref = getServiceExecutorReserved(ctx);

    // The ServiceExecutorReserved could be absent, so nullptr is okay.
    return ref.get();
}

Status ServiceExecutorReserved::shutdown(Milliseconds timeout) {
    LOGV2_DEBUG(22980, 3, "Shutting down reserved executor");

    std::unique_lock<std::mutex> lock(_mutex);
    _stillRunning.store(false);
    _threadWakeup.notify_all();

    bool result = _shutdownCondition.wait_for(lock, timeout.toSystemDuration(), [this]() {
        return _numRunningWorkerThreads.load() == 0;
    });

    return result
        ? Status::OK()
        : Status(ErrorCodes::Error::ExceededTimeLimit,
                 "reserved executor couldn't shutdown all worker threads within time limit.");
}

void ServiceExecutorReserved::_schedule(Task task) {
    if (!_stillRunning.load()) {
        task(Status(ErrorCodes::ShutdownInProgress, "Executor is not running"));
        return;
    }

    if (!_localWorkQueue.empty()) {
        _localWorkQueue.emplace_back(std::move(task));
        return;
    }

    std::lock_guard<std::mutex> lk(_mutex);
    _readyTasks.push_back(std::move(task));
    _threadWakeup.notify_one();
}

void ServiceExecutorReserved::appendStats(BSONObjBuilder* bob) const {
    // The ServiceExecutorReserved loans a thread to one client for its lifetime and waits
    // synchronously on thread.
    struct Statlet {
        int threads;
        int total;
        int running;
        int waiting;
    };

    auto statlet = [&] {
        auto threads = static_cast<int>(_numRunningWorkerThreads.loadRelaxed());
        auto ready = static_cast<int>(_numReadyThreads.loadRelaxed());
        auto starting = static_cast<int>(_numStartingThreads.loadRelaxed());
        // Clamp to 0 in case there is a race condition where ready + starting > threads.
        auto total = std::max(threads - ready - starting, 0);
        auto running = total;
        auto waiting = 0;
        return Statlet{threads, total, running, waiting};
    }();

    BSONObjBuilder subbob = bob->subobjStart(kExecutorName);
    subbob.append(kThreadsRunning, statlet.threads);
    subbob.append(kClientsInTotal, statlet.total);
    subbob.append(kClientsRunning, statlet.running);
    subbob.append(kClientsWaiting, statlet.waiting);
}

/**
 * Schedules task immediately, on the assumption that The task will block to
 * receive the next message and we don't mind blocking on this dedicated
 * worker thread.
 */
void ServiceExecutorReserved::_runOnDataAvailable(const std::shared_ptr<Session>& session,
                                                  Task task) {
    invariant(session);
    _schedule([this, session, callback = std::move(task)](Status status) {
        if (!status.isOK()) {
            callback(std::move(status));
            return;
        }
        callback(session->waitForData());
    });
}

auto ServiceExecutorReserved::makeTaskRunner() -> std::unique_ptr<TaskRunner> {
    iassert(ErrorCodes::ShutdownInProgress, "Executor is not running", _stillRunning.load());

    /** Schedules on this. */
    class ForwardingTaskRunner : public TaskRunner {
    public:
        explicit ForwardingTaskRunner(ServiceExecutorReserved* e) : _e{e} {}

        void schedule(Task task) override {
            _e->_schedule(std::move(task));
        }

        void runTaskForSession(std::shared_ptr<Session> session, Task task) override {
            // Wait for data to become available on session before running task.
            _e->_runOnDataAvailable(std::move(session), std::move(task));
        }

    private:
        ServiceExecutorReserved* _e;
    };
    return std::make_unique<ForwardingTaskRunner>(this);
}

}  // namespace transport
}  // namespace mongo
