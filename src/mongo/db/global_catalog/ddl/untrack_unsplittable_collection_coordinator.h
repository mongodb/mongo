// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/ddl/untrack_unsplittable_collection_coordinator_document_gen.h"
#include "mongo/util/modules.h"

namespace mongo {

class UntrackUnsplittableCollectionCoordinator final
    : public RecoverableShardingDDLCoordinator<UntrackUnsplittableCollectionCoordinatorDocument> {
public:
    UntrackUnsplittableCollectionCoordinator(ShardingCoordinatorService* service,
                                             const BSONObj& initialState)
        : RecoverableShardingDDLCoordinator(
              service, "UntrackUnsplittableCollectionCoordinator", initialState),
          _critSecReason(BSON("command"
                              << "untrackCollection"
                              << "ns"
                              << NamespaceStringUtil::serialize(
                                     originalNss(), SerializationContext::stateDefault()))) {}

    ~UntrackUnsplittableCollectionCoordinator() override = default;

    void checkIfOptionsConflict(const BSONObj& coorDoc) const override;

    void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override;

protected:
    bool isInCriticalSection(Phase phase) const override;

private:
    const BSONObj _critSecReason;

    bool _mustAlwaysMakeProgress() override {
        return _doc.getPhase() > Phase::kUnset;
    }

    void _checkPreconditions();

    void _enterCriticalSection(OperationContext* opCtx,
                               std::shared_ptr<executor::ScopedTaskExecutor> executor,
                               const CancellationToken& token);

    void _commitUntrackCollection(OperationContext* opCtx,
                                  std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token);

    void _exitCriticalSection(OperationContext* opCtx,
                              std::shared_ptr<executor::ScopedTaskExecutor> executor,
                              const CancellationToken& token);

    void _fireAndForgetRefresh();

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;
};
}  // namespace mongo
