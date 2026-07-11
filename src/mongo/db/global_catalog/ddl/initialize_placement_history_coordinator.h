// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/ddl/initialize_placement_history_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/util/modules.h"

namespace mongo {

class InitializePlacementHistoryCoordinator final
    : public RecoverableShardingDDLCoordinator<InitializePlacementHistoryCoordinatorDocument> {
public:
    InitializePlacementHistoryCoordinator(ShardingCoordinatorService* service,
                                          const BSONObj& initialState)
        : RecoverableShardingDDLCoordinator(
              service, "InitializePlacementHistoryCoordinator", initialState) {}

    ~InitializePlacementHistoryCoordinator() override = default;

    void checkIfOptionsConflict(const BSONObj& doc) const final {}

protected:
    bool isInCriticalSection(Phase phase) const override;

private:
    std::set<NamespaceString> _getAdditionalLocksToAcquire(OperationContext* opCtx) override;

    bool _mustAlwaysMakeProgress() override;

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;
};
}  // namespace mongo
