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

#include "mongo/db/ttl_collection_cache.h"
#include "mongo/util/background.h"

namespace mongo {

class ServiceContext;

/**
 * Instantiates the TTLMonitor to periodically remove documents from TTL collections. Safe to call
 * again after shutdownTTLMonitor() has been called.
 */
void startTTLMonitor(ServiceContext* serviceContext);

/**
 * Shuts down the TTLMonitor if it is running. Safe to call multiple times.
 */
void shutdownTTLMonitor(ServiceContext* serviceContext);

class TTLMonitor : public BackgroundJob {
public:
    explicit TTLMonitor() : BackgroundJob(false /* selfDelete */) {}

    static TTLMonitor* get(ServiceContext* serviceCtx);

    static void set(ServiceContext* serviceCtx, std::unique_ptr<TTLMonitor> monitor);

    std::string name() const {
        return "TTLMonitor";
    }

    void run();

    /**
     * Signals the thread to quit and then waits until it does.
     */
    void shutdown();

    /**
     * Invoked when the node enters the primary state.
     */
    void onStepUp(OperationContext* opCtx);

    long long getTTLPasses_forTest();
    long long getTTLSubPasses_forTest();

private:
    friend class TTLTest;

    /**
     * Deletes all expired documents. May consist of several sub-passes.
     */
    void _doTTLPass();

    /**
     * A sub-pass iterates over a list of TTL indexes until there are no more expired documents to
     * delete or 'ttlSubPassTargetSecs' is exceeded.
     *
     * Once it is confirmed there are no more expired documents on an index, the index will not be
     * visited again for the remainder of the sub-pass.
     *
     * Returns true if there are more expired documents to delete. False otherwise.
     */
    bool _doTTLSubPass(OperationContext* opCtx);

    /**
     * Given a TTL index, attempts to delete all expired documents through the index until
     * - hitting the batched delete document or time limit for a single TTL index
     * - removing all expired documents corresponding to the TTLIndex
     * - reaching an error
     *
     * Returns true if there are more expired documents to delete (a batched delete limit is met).
     * Returns false if there are no more expired documents to delete at this time - including when
     * errors specific to the TTL index are reached.
     *
     * When batching is disabled, always returns false since all expired documents are guaranteed to
     * be removed.
     *
     * In some cases (i.e: on temporary resharding collections, collections pending to be dropped,
     * etc), the TTLMonitor is prohibitied from removing expired documents - returns false in these
     * scenarios as well.
     */
    bool _doTTLIndexDelete(OperationContext* opCtx,
                           TTLCollectionCache* ttlCollectionCache,
                           const UUID& uuid,
                           const TTLCollectionCache::Info& info);

    /**
     * Removes documents from the collection using the specified TTL index after a sufficient
     * amount of time has passed according to its expiry specification.
     *
     * Returns true if there are more expired documents to delete through the index at this time.
     * False otherwise.
     *
     * When batching is disabled, always returns false since all expired documents are guaranteed to
     * be removed.
     */
    bool _deleteExpiredWithIndex(OperationContext* opCtx,
                                 TTLCollectionCache* ttlCollectionCache,
                                 const CollectionPtr& collection,
                                 std::string indexName);

    /*
     * Removes expired documents from a clustered collection using a bounded collection scan.
     * On time-series buckets collections, TTL operates on type 'ObjectId'. On general purpose
     * collections, TTL operates on type 'Date'.
     *
     * Returns true if there are more expired documents to delete through the clustered index at
     * this time. False otherwise.
     *
     * When batching is disabled, always returns false since all expired documents are guaranteed to
     * be removed.
     */
    bool _deleteExpiredWithCollscan(OperationContext* opCtx,
                                    TTLCollectionCache* ttlCollectionCache,
                                    const CollectionPtr& collection);

    // Protects the state below.
    mutable Mutex _stateMutex = MONGO_MAKE_LATCH("TTLMonitorStateMutex");

    // Signaled to wake up the thread, if the thread is waiting. The thread will check whether
    // _shuttingDown is set and stop accordingly.
    mutable stdx::condition_variable _shuttingDownCV;

    bool _shuttingDown = false;
};

}  // namespace mongo
