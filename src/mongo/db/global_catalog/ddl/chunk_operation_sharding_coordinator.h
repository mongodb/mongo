// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/ddl/sharding_coordinator.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"

#include <string_view>

namespace mongo {

class [[MONGO_MOD_PRIVATE]] ChunkOperationShardingCoordinatorMixin {
protected:
    virtual ~ChunkOperationShardingCoordinatorMixin() = default;
    void _checkSetAllowChunkOperations(OperationContext* opCtx, const NamespaceString& nss);
};

template <typename StateDoc>
class [[MONGO_MOD_UNFORTUNATELY_OPEN]] ChunkOperationShardingCoordinator
    : public RecoverableShardingCoordinator,
      protected RecoverableTypedDocMixin<ChunkOperationShardingCoordinator<StateDoc>, StateDoc>,
      protected ChunkOperationShardingCoordinatorMixin {

    friend RecoverableTypedDocMixin<ChunkOperationShardingCoordinator<StateDoc>, StateDoc>;

protected:
    explicit ChunkOperationShardingCoordinator(ShardingCoordinatorService* service,
                                               std::string name,
                                               const BSONObj& coorDoc)
        : RecoverableShardingCoordinator(service, std::move(name), coorDoc),
          RecoverableTypedDocMixin<ChunkOperationShardingCoordinator<StateDoc>, StateDoc>(coorDoc) {
    }

    const CoordinatorStateDoc& getDoc() const override {
        return this->_docWrapper;
    }

    CoordinatorStateDoc& getDoc() override {
        return this->_docWrapper;
    }

    /**
     * Returns the statistics bucket this coordinator's lifecycle events are counted under. Each
     * derived chunk-operation coordinator maps to a single operation type.
     */
    virtual ChunkOperationsStatistics::ChunkOperationType chunkOperationMetricType() const = 0;

    /**
     * Counts this coordinator as started, but only on its very first execution. Retries and
     * failover-recovered instances continue an operation that was already counted, so they are
     * skipped. Call this from the coordinator's first phase handler.
     */
    void _registerChunkOperationStarted(OperationContext* opCtx) {
        if (this->_firstExecution) {
            ShardingStatistics::get(opCtx).chunkOperationsStatistics.registerStarted(
                chunkOperationMetricType());
        }
    }

    /**
     * Records the terminal outcome (committed vs aborted) of the operation. Runs exactly once per
     * completion on this node. All chunk-operation coordinators drive aborts through
     * triggerCleanup / _cleanupOnAbort, so a cleanly aborted coordinator carries an abort reason
     * while a committed one does not.
     */
    void _onCleanup(OperationContext* opCtx) override {
        // Preserve the base cleanup (e.g. releasing the retryable-write session).
        RecoverableShardingCoordinator::_onCleanup(opCtx);

        auto& stats = ShardingStatistics::get(opCtx).chunkOperationsStatistics;
        if (this->getAbortReason()) {
            stats.registerAborted(chunkOperationMetricType());
        } else {
            stats.registerCommitted(chunkOperationMetricType());
        }
    }

    /**
     * Returns a token that lets a chunk-operation coordinator acquire ActiveMigrationsRegistry
     * locks without waiting for the sharding coordinator service's own recovery. Required when the
     * acquisition happens inside the coordinator recovery path — waiting on recovery from that
     * context would deadlock, since the caller is itself part of what's holding recovery open.
     *
     * The helper is intentionally `protected` (not public): only the template — which is the sole
     * `BypassRecoveryWait` friend within the coordinator hierarchy — and its derived classes can
     * call it. Unrelated code in the codebase cannot reach a constructed token. Subclasses must
     * still only invoke it from their recovery / lock-acquisition phase; calling it from a
     * steady-state code path (e.g. `_runImpl` after locks are held) re-opens the very window the
     * bypass exists to close, since the bypassed wait is the guard that orders newly-submitted
     * chunk commands behind recovered coordinators across failover.
     */
    static ActiveMigrationsRegistry::BypassRecoveryWait makeRegistryRecoveryBypass() {
        return {};
    }

private:
    void _initialize(OperationContext* opCtx) override {}

    ExecutorFuture<void> _acquireLocksAsync(OperationContext* opCtx,
                                            std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                            const CancellationToken& token) override = 0;

    void _releaseLocks(OperationContext* opCtx) override {}

    std::string_view serializeGenericPhase(CoordinatorGenericPhase phase) const final {
        return this->serializePhase(phase);
    }

    bool _isInCriticalSectionGeneric(CoordinatorGenericPhase phase) const final {
        return this->isInCriticalSection(
            CoordinatorStateDocImpl<StateDoc>::castToCoordinatorPhase(phase));
    }
};

}  // namespace mongo
