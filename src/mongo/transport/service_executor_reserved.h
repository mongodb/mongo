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

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"
#include "mongo/util/duration.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

namespace mongo {
namespace transport {

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

    StringData getName() const override {
        return "ServiceExecutorReserved"_sd;
    }

private:
    Status _startWorker();

    void _schedule(Task task);

    void _runOnDataAvailable(const std::shared_ptr<Session>& session, Task task);

    static thread_local std::deque<Task> _localWorkQueue;
    static thread_local int64_t _localThreadIdleCounter;

    AtomicWord<bool> _stillRunning{false};

    mutable stdx::mutex _mutex;
    stdx::condition_variable _threadWakeup;
    stdx::condition_variable _shutdownCondition;

    std::deque<Task> _readyTasks;

    AtomicWord<unsigned> _numRunningWorkerThreads{0};
    size_t _numReadyThreads{0};
    size_t _numStartingThreads{0};

    const std::string _name;
    const size_t _reservedThreads;
};

}  // namespace transport
}  // namespace mongo
