// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/ddl/migration_blocking_operation_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/util/modules.h"

namespace mongo {

class MigrationBlockingOperationCoordinator
    : public RecoverableShardingDDLCoordinator<MigrationBlockingOperationCoordinatorDocument> {
public:
    using UUIDSet = stdx::unordered_set<UUID, UUID::Hash>;
    using ShardingCoordinator::getOrCreate;

    static std::shared_ptr<MigrationBlockingOperationCoordinator> getOrCreate(
        OperationContext* opCtx, const NamespaceString& nss);
    static boost::optional<std::shared_ptr<MigrationBlockingOperationCoordinator>> get(
        OperationContext* opCtx, const NamespaceString& nss);

    MigrationBlockingOperationCoordinator(ShardingCoordinatorService* service,
                                          const BSONObj& initialState);

    void checkIfOptionsConflict(const BSONObj& stateDoc) const override;

    void beginOperation(OperationContext* opCtx, const UUID& operationUUID);
    void endOperation(OperationContext* opCtx, const UUID& operationUUID);

protected:
    bool isInCriticalSection(Phase phase) const override;

private:
    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    /**
     * Opts out of the causality barrier performed by `run()` on re-execution. Unlike other
     * coordinators, this one does not perform its work inside `_runImpl()` (which merely waits).
     */
    void _performCausalityBarrier(const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                                  const CancellationToken& token) override {}

    Phase _getCurrentPhase() const;
    bool _isFirstOperation(WithLock lk) const;
    void _throwIfCleaningUp(WithLock lk);
    void _recoverIfNecessary(WithLock lk, OperationContext* opCtx, bool isBeginOperation);

    void _insertOrUpdateStateDocument(WithLock lk,
                                      OperationContext* opCtx,
                                      StateDoc newStateDocument);

    mutable std::mutex _mutex;

    UUIDSet _operations;
    SharedPromise<void> _beginCleanupPromise;
    bool _needsRecovery;
};

}  // namespace mongo
