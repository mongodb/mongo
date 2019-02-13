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

#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {

class NamespaceString;
class OperationContext;
class ServiceContext;
class ChunkSplitStateDriver;

/**
 * Handles asynchronous auto-splitting of chunks.
 */
class ChunkSplitter {
    MONGO_DISALLOW_COPYING(ChunkSplitter);

public:
    ChunkSplitter();
    ~ChunkSplitter();

    /**
     * Obtains the service-wide chunk splitter instance.
     */
    static ChunkSplitter& get(OperationContext* opCtx);
    static ChunkSplitter& get(ServiceContext* serviceContext);

    /**
     * Sets the mode of the ChunkSplitter to either primary or secondary.
     * The ChunkSplitter is only active when primary.
     */
    void onShardingInitialization(bool isPrimary);

    /**
     * Invoked when the shard server primary enters the 'PRIMARY' state to set up the ChunkSplitter
     * to begin accepting split requests.
     */
    void onStepUp();

    /**
     * Invoked when this node which is currently serving as a 'PRIMARY' steps down.
     *
     * This method might be called multiple times in succession, which is what happens as a result
     * of incomplete transition to primary so it is resilient to that.
     */
    void onStepDown();

    /**
     * Blocks until all chunk split tasks in the underlying thread pool have
     * completed (that is, until the thread pool is idle)
     */
    void waitForIdle();

    /**
     * Schedules an autosplit task. This function throws on scheduling failure.
     */
    void trySplitting(std::shared_ptr<ChunkSplitStateDriver> chunkSplitStateDriver,
                      const NamespaceString& nss,
                      const BSONObj& min,
                      const BSONObj& max,
                      long dataWritten);

private:
    /**
     * Determines if the specified chunk should be split and then performs any necessary splits.
     *
     * It may also perform a 'top chunk' optimization where a resulting chunk that contains either
     * MaxKey or MinKey as a range extreme will be moved off to another shard to relieve load on the
     * original owner. This optimization presumes that the user is doing writes with increasing or
     * decreasing shard key values.
     */
    void _runAutosplit(std::shared_ptr<ChunkSplitStateDriver> chunkSplitStateDriver,
                       const NamespaceString& nss,
                       const BSONObj& min,
                       const BSONObj& max,
                       long dataWritten);

    // Protects the state below.
    stdx::mutex _mutex;

    // The ChunkSplitter is only active on a primary node.
    bool _isPrimary{false};

    // Thread pool for parallelizing splits.
    ThreadPool _threadPool;
};

}  // namespace mongo
