/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/s/remove_shard_commit_coordinator.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

ExecutorFuture<void> RemoveShardCommitCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kJoinMigrationsAndCheckRangeDeletions,
            [this, anchor = shared_from_this()] { return _doc.getIsTransitionToDedicated(); },
            [this, executor = executor, anchor = shared_from_this()] {
                _joinMigrationsAndCheckRangeDeletions();
            }))
        .then([this] {
            uasserted(ErrorCodes::NotImplemented,
                      "The removeShardCommit coordinator is still incomplete.");
        })
        .onError([](const Status& status) { return status; });
}

void RemoveShardCommitCoordinator::_joinMigrationsAndCheckRangeDeletions() {
    auto opCtxHolder = cc().makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    getForwardableOpMetadata().setOn(opCtx);

    topology_change_helpers::joinMigrations(opCtx);
    // The config server may be added as a shard again, so we locally drop its drained
    // sharded collections to enable that without user intervention. But we have to wait for
    // the range deleter to quiesce to give queries and stale routers time to discover the
    // migration, to match the usual probabilistic guarantees for migrations.
    auto pendingRangeDeletions = topology_change_helpers::getRangeDeletionCount(opCtx);
    if (pendingRangeDeletions > 0) {
        LOGV2(9782400,
              "removeShard: waiting for range deletions",
              "pendingRangeDeletions"_attr = pendingRangeDeletions);
        uasserted(
            ErrorCodes::ChunkRangeCleanupPending,
            "Range deletions must complete before transitioning to a dedicated config server.");
    }
}

RemoveShardProgress RemoveShardCommitCoordinator::getResult(OperationContext* opCtx) {
    getCompletionFuture().get(opCtx);
    invariant(_result.is_initialized());
    return *_result;
}

}  // namespace mongo
