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

#include "mongo/db/sharding_environment/shard_shared_state_cache.h"

#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_retry_server_parameters_gen.h"
#include "mongo/stdx/mutex.h"

#include <memory>
#include <shared_mutex>

namespace mongo {
namespace {

const auto getShardSharedState = ServiceContext::declareDecoration<ShardSharedStateCache>();

}

ShardSharedStateCache& ShardSharedStateCache::get(ServiceContext* serviceContext) {
    invariant(serviceContext);
    return getShardSharedState(serviceContext);
}

ShardSharedStateCache& ShardSharedStateCache::get(OperationContext* opCtx) {
    invariant(opCtx);
    return get(opCtx->getServiceContext());
}

Status ShardSharedStateCache::updateRetryBudgetReturnRate(double returnRate) {
    if (auto client = Client::getCurrent()) {
        auto* opCtx = client->getOperationContext();
        auto& shardSharedStateCache = get(opCtx);

        const auto capacity = gShardRetryTokenBucketCapacity.load();
        shardSharedStateCache._updateRetryBudgetRateParameters(returnRate, capacity);
    }

    return Status::OK();
}

Status ShardSharedStateCache::updateRetryBudgetCapacity(std::int32_t capacity) {
    if (auto client = Client::getCurrent()) {
        auto* opCtx = client->getOperationContext();
        auto& shardSharedStateCache = get(opCtx);

        const auto returnRate = gShardRetryTokenReturnRate.load();
        shardSharedStateCache._updateRetryBudgetRateParameters(returnRate, capacity);
    }

    return Status::OK();
}

void ShardSharedStateCache::forgetShardState(const ShardId& shardId) {
    stdx::unique_lock _{_mutex};
    _shardStateById.erase(shardId);
}

auto ShardSharedStateCache::getShardState(const ShardId& shardId) -> std::shared_ptr<State> {
    {
        std::shared_lock _{_mutex};
        if (auto it = _shardStateById.find(shardId); it != _shardStateById.end()) {
            return it->second;
        }
    }

    stdx::unique_lock _{_mutex};
    const auto [it, inserted] = _shardStateById.try_emplace(
        shardId,
        std::make_shared<State>(gShardRetryTokenReturnRate.loadRelaxed(),
                                gShardRetryTokenBucketCapacity.loadRelaxed()));
    return it->second;
}

void ShardSharedStateCache::_updateRetryBudgetRateParameters(double returnRate, double capacity) {
    auto latestShardStateById = [&] {
        std::shared_lock _{_mutex};
        return _shardStateById;
    }();

    // Any new shared states added while we iterate here will have the new server parameters
    // already, so they are safe to ignore.
    for (auto& [_, sharedState] : latestShardStateById) {
        sharedState->retryBudget.updateRateParameters(returnRate, capacity);
    }
}

void ShardSharedStateCache::report(BSONObjBuilder* bob) const {
    auto latestShardStateById = [&] {
        std::shared_lock _{_mutex};

        std::vector<std::pair<ShardId, std::shared_ptr<State>>> latestShardStateById{
            _shardStateById.begin(),
            _shardStateById.end(),
        };

        return latestShardStateById;
    }();

    std::ranges::sort(
        latestShardStateById, std::less{}, [](const auto& pair) { return pair.first; });

    for (const auto& [shardId, state] : latestShardStateById) {
        BSONObjBuilder shardBob = bob->subobjStart(shardId.toString());
        state->stats.appendStats(bob);
        state->retryBudget.appendStats(bob);
    }
}

void ShardSharedStateCache::Stats::appendStats(BSONObjBuilder* bob) const {
    bob->append("numOperationsAttempted", numOperationsAttempted.loadRelaxed());
    bob->append("numOperationsRetriedAtLeastOnceDueToOverload",
                numOperationsRetriedAtLeastOnceDueToOverload.loadRelaxed());
    bob->append("numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded",
                numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded.loadRelaxed());
    bob->append("numRetriesDueToOverloadAttempted", numRetriesDueToOverloadAttempted.loadRelaxed());
    bob->append("numRetriesRetargetedDueToOverload",
                numRetriesRetargetedDueToOverload.loadRelaxed());
    bob->append("numOverloadErrorsReceived", numOverloadErrorsReceived.loadRelaxed());
    bob->append("totalBackoffTimeMillis", totalBackoffTimeMillis.loadRelaxed());
}

}  // namespace mongo
