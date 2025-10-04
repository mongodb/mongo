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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace MONGO_MOD_PUB mongo {
namespace executor {

/**
 * Represents a pool of TaskExecutors. Work which requires a TaskExecutor can ask for an executor
 * from the pool. This allows for work to be distributed across several executors.
 *
 * The TaskExecutor and its owned NetworkInterface can become a performance bottleneck, so this
 * pooling approach provides better scalability under concurrent query workloads.
 *
 * Initialization and shutdown methods are not thread-safe, but getArbitraryExecutor() may be called
 * concurrently by multiple threads.
 */
class TaskExecutorPool final {
public:
    /**
     * Based on properties of the system (e.g. the number of cores), suggests a number of executors
     * to add to the pool.
     */
    static size_t getSuggestedPoolSize();

    /**
     * Initializes the underlying executors. This method may be called at most once for the lifetime
     * of an executor.
     *
     * May only be called after populating the pool with addExecutors().
     */
    void startup();

    /**
     * Shuts down all underlying executors and waits for them to finish. May block.
     *
     * If startup() has been called, this method must eventually be called exactly once. Must be
     * called after startup().
     */
    void shutdownAndJoin();

    MONGO_MOD_PUB void shutdown_forTest();
    MONGO_MOD_PUB void join_forTest();

    /**
     * Adds 'executors' and 'fixedExecutor' to the pool. May be called at most once to initialize an
     * empty pool.
     */
    void addExecutors(std::vector<std::shared_ptr<TaskExecutor>> executors,
                      std::shared_ptr<TaskExecutor> fixedExecutor);

    /**
     * Returns a pointer to one of the executors in the pool. Two calls to this method may return
     * different executors. Invalid to call if the pool has not been initialized with
     * addExecutors().
     *
     * Use this method if you need a TaskExecutor for performing performance-critical work.
     *
     * Thread-safe.
     */
    const std::shared_ptr<TaskExecutor>& getArbitraryExecutor();

    /**
     * Returns a pointer to the pool's fixed executor. Every call to this method will return the
     * same executor. Invalid to call if the fixed executor has not been initialized with
     * addExecutors().
     *
     * Use this method if you are *not* using the TaskExecutor for performance-critical work.
     *
     * Thread-safe.
     */
    const std::shared_ptr<TaskExecutor>& getFixedExecutor();

    /**
     * Appends connection information from all of the executors in the pool.
     *
     * NOTE: this method returns approximate stats. To avoid blocking operations on the
     * pool, we don't lock for appendConnectionStats, so data gathered across connection pools
     * will be from slightly different points in time.
     */
    void appendConnectionStats(ConnectionPoolStats* stats) const;

    /**
     * Appends statistics for all the executors, in particular their underlying network interfaces,
     * in the pool. The information is collected in a non-blocking fashion and is just an
     * approximate.
     */
    void appendNetworkInterfaceStats(BSONObjBuilder&) const;

private:
    AtomicWord<unsigned> _counter;

    std::vector<std::shared_ptr<TaskExecutor>> _executors;

    std::shared_ptr<TaskExecutor> _fixedExecutor;
};

}  // namespace executor
}  // namespace MONGO_MOD_PUB mongo
