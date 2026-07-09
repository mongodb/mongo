/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/sharding_coordinator.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"

#include <string_view>

namespace mongo {

class MONGO_MOD_PRIVATE ChunkOperationShardingCoordinatorMixin {
protected:
    virtual ~ChunkOperationShardingCoordinatorMixin() = default;
    void _checkSetAllowChunkOperations(OperationContext* opCtx, const NamespaceString& nss);
};

template <typename StateDoc>
class MONGO_MOD_UNFORTUNATELY_OPEN ChunkOperationShardingCoordinator
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
