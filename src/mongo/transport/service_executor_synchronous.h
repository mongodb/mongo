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

#include <memory>
#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/service_executor.h"
#include "mongo/util/duration.h"

namespace mongo::transport {

/**
 * Transitional for differential benchmarking of ServiceExecutorSynchronous refactor.
 * Can be removed when it's no longer necessary to benchmark the refactor.
 * Picked up by service_executor_bm.cpp to detect this newer API.
 */
#define TRANSITIONAL_SERVICE_EXECUTOR_SYNCHRONOUS_HAS_RESERVE 1

/**
 * ServiceExecutorSynchronous is just a special case of ServiceExecutorReserved,
 * Which just happens to have `reservedTheads == 0`. Both are implemented by
 * this base class.
 *
 * It will start `reservedThreads` on start, and create enough new threads every
 * time it gives out a lease, that there are at least `reservedThreads`
 * available for work (as long as spawning is possible). This means that even
 * when we hit the NPROC ulimit, and spawning is temporarily failing, there will
 * still be threads ready to accept work.
 *
 * When a lease is released, the worker will go back to waiting for work if
 * there are no more than `reservedThreads + maxIdleThreads` available.
 * Otherwise it will be destroyed. This means that `reservedThreads` are for
 * emergency use only, while the `maxIdleThreads` specifies the strength of an
 * optimization that minimizes the churn of worker threads. With
 * `maxIdleThreads==0`, every worker lease would spawn a new thread, which is
 * very wasteful.
 */
class ServiceExecutorSyncBase : public ServiceExecutor {
public:
    ~ServiceExecutorSyncBase() override;

    Status start() override;
    Status shutdown(Milliseconds timeout) override;

    std::unique_ptr<Executor> makeTaskRunner() override;

    size_t getRunningThreads() const override;

    void appendStats(BSONObjBuilder* bob) const override;

protected:
    /**
     * `reservedThreads` - At `start` time and at `makeTaskRunner` time, workers
     * will be spawned until this number of available threads is reached. This
     * is intended as a minimum bulwark against periods during which workers
     * cannot spawn.
     *
     * `maxIdleThreads` - When a worker lease is released, the corresponding
     * worker can be destroyed, or it can be kept for use by the next lease.
     * This parameter specifies the upper limit on the number of such kept
     * threads, not counting reserved threads.
     */
    ServiceExecutorSyncBase(std::string name,
                            size_t reservedThreads,
                            size_t maxIdleThreads,
                            std::string statLabel);

private:
    class Impl;

    std::shared_ptr<Impl> _impl;
};

/**
 * Provides access to a pool of dedicated worker threads via `makeTaskRunner`,
 * which retuns a worker lease. A worker lease is a handle that acts as an
 * executor, accepting tasks. The leased worker thread is dedicated, meaning it
 * will only work through the lease handle: all tasks scheduled on that
 * lease will be run on that worker, and no other tasks.
 *
 * A worker thread may be reused and leased many times, but only serially so.
 */
class ServiceExecutorSynchronous : public ServiceExecutorSyncBase {
public:
    static inline size_t defaultReserved = 0; /** For testing */

    /** Returns the ServiceExecutorSynchronous decoration on `ctx`. */
    static ServiceExecutorSynchronous* get(ServiceContext* ctx);

    ServiceExecutorSynchronous();
};

/**
 * Same as ServiceExecutorSynchronous (implemented by the same base class), but
 * ServiceExecutorReserved has the additional property of having configurable
 * `reservedThreads` and `maxIdleThreads` counts. See base class docs.
 */
class ServiceExecutorReserved : public ServiceExecutorSyncBase {
public:
    /** Null result is possible, as ServiceExecutorReserved could be absent. */
    static ServiceExecutorReserved* get(ServiceContext* ctx);

    ServiceExecutorReserved(std::string name, size_t reservedThreads, size_t maxIdleThreads);
};

}  // namespace mongo::transport
