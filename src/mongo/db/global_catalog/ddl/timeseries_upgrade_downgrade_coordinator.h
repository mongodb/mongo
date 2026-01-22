/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

// TODO (SERVER-116499): Remove this file once 9.0 becomes last LTS.

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
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
    : public RecoverableShardingDDLCoordinator<TimeseriesUpgradeDowngradeCoordinatorDocument,
                                               TimeseriesUpgradeDowngradeCoordinatorPhaseEnum> {
public:
    using StateDoc = TimeseriesUpgradeDowngradeCoordinatorDocument;
    using Phase = TimeseriesUpgradeDowngradeCoordinatorPhaseEnum;

    TimeseriesUpgradeDowngradeCoordinator(ShardingDDLCoordinatorService* service,
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

private:
    StringData serializePhase(const Phase& phase) const override {
        return TimeseriesUpgradeDowngradeCoordinatorPhase_serializer(phase);
    }

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
