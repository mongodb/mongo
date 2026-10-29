// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// TODO (SERVER-116499): Remove this file once 9.0 becomes last LTS.

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/ddl/timeseries_upgrade_downgrade_coordinator_document_gen.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class TimeseriesUpgradeDowngradeCoordinator final
    : public RecoverableShardingDDLCoordinator<TimeseriesUpgradeDowngradeCoordinatorDocument> {
public:
    TimeseriesUpgradeDowngradeCoordinator(ShardingCoordinatorService* service,
                                          const BSONObj& initialState)
        : RecoverableShardingDDLCoordinator(
              service, "TimeseriesUpgradeDowngradeCoordinator", initialState),
          _request(_doc.getTimeseriesUpgradeDowngradeRequest()),
          _critSecReason(
              BSON("upgradeDowngradeViewlessTimeseries" << NamespaceStringUtil::serialize(
                       originalNss(), SerializationContext::stateDefault()))) {}

    void checkIfOptionsConflict(const BSONObj& doc) const override;

protected:
    logv2::DynamicAttributes getCoordinatorLogAttrs() const override;

    bool isInCriticalSection(Phase phase) const override;

private:
    bool _mustAlwaysMakeProgress() override {
        return _doc.getPhase() >= Phase::kFreezeMigrations;
    };

    void _checkPreconditions(OperationContext* opCtx);

    /**
     * Checks if the collection is tracked in config.collections and persists the result.
     */
    void _determineIsTracked(OperationContext* opCtx);

    /**
     * Returns whether the collection is tracked in config.collections.
     * Must be called after _determineIsTracked has run.
     */
    bool _isTracked() const;

    /**
     * Releases the critical section on the given namespace on all participating shards.
     */
    void _releaseCriticalSectionFor(OperationContext* opCtx,
                                    std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                    const CancellationToken& token,
                                    const std::vector<ShardId>& participants,
                                    const NamespaceString& nss);

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    ExecutorFuture<void> _cleanupOnAbort(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token,
                                         const Status& status) noexcept override;

    const TimeseriesUpgradeDowngradeRequest _request;
    const BSONObj _critSecReason;
};

}  // namespace mongo
