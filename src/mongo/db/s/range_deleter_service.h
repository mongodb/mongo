/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/util/future_util.h"

namespace mongo {

// TODO SERVER-67636 make RangeDeleterService a ReplicaSetAwareServiceShardsvr
class RangeDeleterService {
public:
    RangeDeleterService() {
        // TODO SERVER-67636 move executor's initialization at replica set aware service level
        const std::string kExecName("RangeDeleterServiceExecutor");
        auto net = executor::makeNetworkInterface(kExecName);
        auto pool = std::make_unique<executor::NetworkInterfaceThreadPool>(net.get());
        auto taskExecutor =
            std::make_shared<executor::ThreadPoolTaskExecutor>(std::move(pool), std::move(net));
        taskExecutor->startup();
        _executor = std::move(taskExecutor);
    }

    ~RangeDeleterService() {
        // TODO SERVER-67636 move executor's shutdown at replica set aware service level
        _executor->shutdown();
        _executor->join();
    }

private:
    /*
     * In memory representation of registered range deletion tasks. To each non-pending range
     * deletion task corresponds a registered task on the service.
     */
    class RangeDeletion : public ChunkRange {
    public:
        RangeDeletion(const RangeDeletionTask& task, SharedSemiFuture<void> completion)
            : ChunkRange(task.getRange().getMin(), task.getRange().getMax()),
              _completion(completion) {}

        SharedSemiFuture<void> getCompletionFuture() const {
            return _completion;
        }

    private:
        // Marked ready once the range deletion has been fully processed
        const SharedSemiFuture<void> _completion;
    };

    /*
     * Internal comparator to sort ranges in _rangeDeletionTasks's sets.
     *
     * NB: it ONLY makes sense to use this on ranges that are comparable, meaning
     * the ones based on the same key pattern (aka the ones belonging to the same
     * sharded collection).
     */
    struct RANGES_COMPARATOR {
        bool operator()(const std::shared_ptr<ChunkRange>& a,
                        const std::shared_ptr<ChunkRange>& b) const {
            return a->getMin().woCompare(b->getMin()) < 0;
        }
    };

    // Keeping track of per-collection registered range deletion tasks
    stdx::unordered_map<UUID, std::set<std::shared_ptr<ChunkRange>, RANGES_COMPARATOR>, UUID::Hash>
        _rangeDeletionTasks;

    // Mono-threaded executor processing range deletion tasks
    std::shared_ptr<executor::TaskExecutor> _executor;

    // TODO SERVER-67642 implement fine-grained per-collection locking
    // Protecting the access to all class members
    Mutex _mutex = MONGO_MAKE_LATCH("RangeDeleterService::_mutex");

public:
    /*
     * Register a task on the range deleter service.
     * Returns a future that will be marked ready once the range deletion will be completed.
     *
     * In case of trying to register an already existing task, the future will contain an error.
     */
    SharedSemiFuture<void> registerTask(
        const RangeDeletionTask& rdt,
        SemiFuture<void>&& waitForActiveQueriesToComplete = SemiFuture<void>::makeReady());

    /*
     * Deregister a task from the range deleter service.
     */
    void deregisterTask(const UUID& collUUID, const ChunkRange& range);

    /*
     * Returns the number of registered range deletion tasks for a collection
     */
    int getNumRangeDeletionTasksForCollection(const UUID& collectionUUID);
};

}  // namespace mongo
