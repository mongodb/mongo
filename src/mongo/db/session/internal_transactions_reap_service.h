// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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

    void onStartup(OperationContext* opCtx) final;
    void onSetCurrentConfig(OperationContext* opCtx) final {}
    void onShutdown() final;

    void onStepUpComplete(OperationContext* opCtx, long long term) final {
        std::lock_guard lg(_mutex);
        _enabled = true;
    }

    void onStepDown() final {
        std::lock_guard lg(_mutex);
        _enabled = false;
        _lsidsToEagerlyReap.clear();
    }

    void onConsistentDataAvailable(OperationContext* opCtx,
                                   bool isMajority,
                                   bool isRollback) final {}
    void onStepUpBegin(OperationContext* opCtx, long long term) final {}
    void onRollbackBegin() final {}
    void onBecomeArbiter() final {}
    inline std::string getServiceName() const final {
        return "InternalTransactionsReapService";
    }

    std::shared_ptr<ThreadPool> _threadPool;

    // Protects the state below.
    mutable std::mutex _mutex;

    bool _enabled{false};
    boost::optional<ExecutorFuture<void>> _drainedSessionsFuture;
    std::vector<LogicalSessionId> _lsidsToEagerlyReap;
};

}  // namespace mongo
