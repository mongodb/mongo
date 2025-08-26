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

#include "mongo/db/global_catalog/ddl/initialize_placement_history_coordinator.h"

#include "mongo/db/global_catalog/ddl/placement_history_cleaner.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {}  // namespace

ExecutorFuture<void> InitializePlacementHistoryCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, anchor = shared_from_this()] {
            auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            // TODO SERVER-109079: Reject requests when the config server isn't running under a
            // compatible FCV version.

            // Ensure that there is no concurrent access from the periodic cleaning job (which may
            // have been re-activated during the execution of this Coordinator during a node step
            // up).
            PlacementHistoryCleaner::get(opCtx)->pause();
        })
        .then(_buildPhaseHandler(Phase::kExecute,
                                 [](auto* opCtx) {
                                     ShardingCatalogManager::get(opCtx)->initializePlacementHistory(
                                         opCtx);
                                 }))
        .then(_buildPhaseHandler(Phase::kPostExecution, [](auto* opCtx) {
            PlacementHistoryCleaner::get(opCtx)->resume(opCtx);
        }));
}


}  // namespace mongo
