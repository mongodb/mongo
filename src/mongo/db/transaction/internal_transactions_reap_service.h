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

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {

/**
 * Service responsible for removing completed internal transactions persisted metadata when they can
 * no longer be used. Only enabled on replica set primary nodes.
 */
class InternalTransactionsReapService
    : public ReplicaSetAwareService<InternalTransactionsReapService> {
public:
    InternalTransactionsReapService();

    static InternalTransactionsReapService* get(ServiceContext* serviceContext);
    static InternalTransactionsReapService* get(OperationContext* opCtx);

    /**
     * Adds the eagerly reaped sessions to the reap service to be buffered and later reaped. Does
     * not verify they have actually expired, so callers must guarantee they are safe to remove.
     */
    static void onEagerlyReapedSessions(ServiceContext* service,
                                        std::vector<LogicalSessionId> lsidsToRemove);

    /**
     * Buffers the sessions to be reaped when the max batch size has been reached, upon which a task
     * is scheduled to reap the transactions.
     */
    void addEagerlyReapedSessions(ServiceContext* service,
                                  std::vector<LogicalSessionId> lsidsToRemove);

    /**
     * Used in tests to wait for a kicked off drain to complete.
     */
    void waitForCurrentDrain_forTest();

    /**
     * Used in tests to detect when the drain task is running.
     */
    bool hasCurrentDrain_forTest();

private:
    /**
     * Will remove all currently buffered sessions ids from config.transactions and
     * config.image_collection.
     */
    void _reapInternalTransactions(ServiceContext* service);

    void onStartup(OperationContext* opCtx) override final;
    void onSetCurrentConfig(OperationContext* opCtx) final {}
    void onShutdown() override final;

    void onStepUpComplete(OperationContext* opCtx, long long term) override final {
        stdx::lock_guard lg(_mutex);
        _enabled = true;
    }

    void onStepDown() override final {
        stdx::lock_guard lg(_mutex);
        _enabled = false;
        _lsidsToEagerlyReap.clear();
    }

    void onInitialDataAvailable(OperationContext* opCtx,
                                bool isMajorityDataAvailable) override final {}
    void onStepUpBegin(OperationContext* opCtx, long long term) override final {}
    void onBecomeArbiter() override final {}

    std::shared_ptr<ThreadPool> _threadPool;

    // Protects the state below.
    mutable Mutex _mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(1),
                                            "InternalTransactionsReapService::_mutex");

    bool _enabled{false};
    boost::optional<ExecutorFuture<void>> _drainedSessionsFuture;
    std::vector<LogicalSessionId> _lsidsToEagerlyReap;
};

}  // namespace mongo
