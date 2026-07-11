// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"
#include "mongo/util/duration.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace mongo {
namespace transport {
using namespace std::literals::string_view_literals;

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
class ServiceExecutorReserved final : public ServiceExecutor {
public:
    explicit ServiceExecutorReserved(ServiceContext* ctx, std::string name, size_t reservedThreads);

    static ServiceExecutorReserved* get(ServiceContext* ctx);

    void start() override;
    Status shutdown(Milliseconds timeout) override;

    size_t getRunningThreads() const override {
        return _numRunningWorkerThreads.loadRelaxed();
    }

    void appendStats(BSONObjBuilder* bob) const override;

    std::unique_ptr<TaskRunner> makeTaskRunner() override;

    std::string_view getName() const override {
        return "ServiceExecutorReserved"sv;
    }

private:
    Status _startWorker();

    void _schedule(Task task);

    void _runOnDataAvailable(const std::shared_ptr<Session>& session, Task task);

    static thread_local std::deque<Task> _localWorkQueue;
    static thread_local int64_t _localThreadIdleCounter;

    Atomic<bool> _stillRunning{false};

    mutable std::mutex _mutex;
    stdx::condition_variable _threadWakeup;
    stdx::condition_variable _shutdownCondition;

    std::deque<Task> _readyTasks;

    Atomic<unsigned> _numRunningWorkerThreads{0};
    Atomic<size_t> _numReadyThreads{0};
    Atomic<size_t> _numStartingThreads{0};

    const std::string _name;
    const size_t _reservedThreads;
};

}  // namespace transport
}  // namespace mongo
