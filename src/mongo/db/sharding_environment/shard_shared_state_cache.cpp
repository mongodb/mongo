// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sharding_environment/shard_shared_state_cache.h"

#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_retry_server_parameters_gen.h"

#include <memory>
#include <mutex>
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
    std::unique_lock _{_mutex};
    _shardStateById.erase(shardId);
}

auto ShardSharedStateCache::getShardState(const ShardId& shardId) -> std::shared_ptr<State> {
    {
        std::shared_lock _{_mutex};
        if (auto it = _shardStateById.find(shardId); it != _shardStateById.end()) {
            return it->second;
        }
    }

    std::unique_lock _{_mutex};
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
