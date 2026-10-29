// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_role_loop.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/routing_cache/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_api_d_params_gen.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {

using shard_role_loop::CanRetry;
using shard_role_loop::RetryContext;

CanRetry handleStaleDbVersionError(OperationContext* opCtx,
                                   const StaleDbRoutingVersion& staleDbError,
                                   RetryContext& retryCtx) {
    const bool shardIsStale = staleDbError.getCriticalSectionSignal() ||
        !staleDbError.getVersionWanted() ||
        staleDbError.getVersionWanted() < staleDbError.getVersionReceived();

    const auto& dbName = staleDbError.getDb();
    retryCtx.noteDbRefresh(dbName);
    const bool anyRefreshAttemptRemaining = retryCtx.anyDbRefreshAttemptRemaining(dbName);

    if (shardIsStale) {
        // Recover ShardRole metadata and retry
        const auto& staleExceptionHandler =
            DatabaseShardingState::getStaleShardExceptionHandler(opCtx);
        const auto dbVersionAfterRecovery =
            staleExceptionHandler.handleStaleDatabaseVersionException(opCtx, staleDbError);
        if (!anyRefreshAttemptRemaining) {
            return CanRetry::NO_BECAUSE_EXHAUSTED_RETRIES;
        }
        return dbVersionAfterRecovery &&
                (dbVersionAfterRecovery == staleDbError.getVersionReceived())
            ? CanRetry::YES
            : CanRetry::NO;
    } else {
        // The router is stale. Do not retry at the Shard level. Let the exception propagate up to
        // the router instead.
        return CanRetry::NO;
    }
}

CanRetry handleStaleShardVersionError(OperationContext* opCtx,
                                      const StaleConfigInfo& sci,
                                      RetryContext& retryCtx) {
    ShardingStatistics::get(opCtx).countStaleConfigErrors.addAndFetch(1);

    const auto& nss = sci.getNss();
    retryCtx.noteShardVersionRefresh(nss);
    const bool anyRefreshAttemptRemaining = retryCtx.anyShardVersionRefreshAttemptRemaining(nss);

    enum class ShardIsStale { YES, MAYBE, NO };

    const ShardIsStale shardIsStale = [&]() {
        if (sci.getCriticalSectionSignal()) {
            // If the critical section is active, we need to recover the metadata on this shard (and
            // wait for the critical section to finish first).
            return ShardIsStale::YES;
        }

        if (!sci.getVersionWanted()) {
            // If the shard doesn't know what version is its current one, then it needs to recover
            // it.
            return ShardIsStale::YES;
        }

        // If the version wanted by the shard is older than the one the router sent, the shard is
        // stale. If the two versions are not comparable, pessimistically assume the shard needs
        // refresh.
        invariant(sci.getVersionWanted());
        const auto cmpWantedVsReceived = sci.getVersionWanted()->placementVersion() <=>
            sci.getVersionReceived().placementVersion();
        if (cmpWantedVsReceived == std::partial_ordering::less) {
            return ShardIsStale::YES;
        } else if (cmpWantedVsReceived == std::partial_ordering::unordered) {
            return ShardIsStale::MAYBE;
        } else {
            return ShardIsStale::NO;
        }
    }();

    const auto& staleExceptionHandler =
        CollectionShardingState::getStaleShardExceptionHandler(opCtx);
    switch (shardIsStale) {
        case ShardIsStale::YES: {
            // Refresh the metadata on this shard and retry.
            staleExceptionHandler.handleStaleShardVersionException(opCtx, sci);
            return anyRefreshAttemptRemaining ? CanRetry::YES
                                              : CanRetry::NO_BECAUSE_EXHAUSTED_RETRIES;
        }
        case ShardIsStale::MAYBE: {
            // The shard may or may not be stale. Pessimistically refresh, then check if the new
            // refreshed version has changed respect to the 'wanted' version in the original
            // exception. If so, retry.
            const auto refreshRet =
                staleExceptionHandler.handleStaleShardVersionException(opCtx, sci);
            return refreshRet && sci.getVersionWanted() &&
                    *refreshRet != sci.getVersionWanted()->placementVersion()
                ? (anyRefreshAttemptRemaining ? CanRetry::YES
                                              : CanRetry::NO_BECAUSE_EXHAUSTED_RETRIES)
                : CanRetry::NO;
        }
        case ShardIsStale::NO: {
            // The router is stale. Do not retry at the Shard level. Let the exception propagate up
            // to the router instead.
            return CanRetry::NO;
        }
    }
    MONGO_UNREACHABLE;
}

CanRetry handleShardCannotRefreshDueToLocksHeldError(
    OperationContext* opCtx,
    const ShardCannotRefreshDueToLocksHeldInfo& refreshInfo,
    RetryContext& retryCtx) {
    const auto& nss = refreshInfo.getNss();
    retryCtx.noteCatalogCacheRefresh(nss);
    const bool anyRefreshAttemptRemaining = retryCtx.anyCatalogCacheRefreshAttemptRemaining(nss);

    uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, refreshInfo.getNss()));
    // May be retried.
    return anyRefreshAttemptRemaining ? CanRetry::YES : CanRetry::NO_BECAUSE_EXHAUSTED_RETRIES;
}

}  // namespace

namespace shard_role_loop {

bool RetryContext::anyDbRefreshAttemptRemaining(const DatabaseName& dbName) const {
    const auto it = _dbRefreshCountMap.find(dbName);
    return it == _dbRefreshCountMap.end() ||
        it->second <= maxShardStaleMetadataRetryAttempts.loadRelaxed();
}

bool RetryContext::anyShardVersionRefreshAttemptRemaining(const NamespaceString& nss) const {
    const auto it = _shardVersionRefreshCountMap.find(nss);
    return it == _shardVersionRefreshCountMap.end() ||
        it->second <= maxShardStaleMetadataRetryAttempts.loadRelaxed();
}

bool RetryContext::anyCatalogCacheRefreshAttemptRemaining(const NamespaceString& nss) const {
    const auto it = _catalogCacheRefreshCountMap.find(nss);
    return it == _catalogCacheRefreshCountMap.end() ||
        it->second <= maxShardStaleMetadataRetryAttempts.loadRelaxed();
}

void RetryContext::noteDbRefresh(const DatabaseName& dbName) {
    _dbRefreshCountMap[dbName]++;
}

void RetryContext::noteShardVersionRefresh(const NamespaceString& nss) {
    _shardVersionRefreshCountMap[nss]++;
}

void RetryContext::noteCatalogCacheRefresh(const NamespaceString& nss) {
    _catalogCacheRefreshCountMap[nss]++;
}

CanRetry handleStaleError(OperationContext* opCtx, const Status& error, RetryContext& retryCtx) {
    if (shard_role_details::getLocker(opCtx)->isLocked() ||
        opCtx->getClient()->isInDirectClient()) {
        // Even though the ShardRole metadata might need recovery, we cannot recover it here because
        // locks are held. Let the exception propagate up to a layer where it is allowed to recover
        // the metadata, if needed.
        return CanRetry::NO;
    }

    switch (error.code()) {
        case ErrorCodes::StaleDbVersion: {
            const auto staleDbError = error.extraInfo<StaleDbRoutingVersion>();
            invariant(staleDbError);
            return handleStaleDbVersionError(opCtx, *staleDbError, retryCtx);
        }
        case ErrorCodes::StaleConfig: {
            const auto staleConfigError = error.extraInfo<StaleConfigInfo>();
            invariant(staleConfigError);
            return handleStaleShardVersionError(opCtx, *staleConfigError, retryCtx);
        }
        case ErrorCodes::ShardCannotRefreshDueToLocksHeld: {
            const auto extraInfo = error.extraInfo<ShardCannotRefreshDueToLocksHeldInfo>();
            invariant(extraInfo);
            return handleShardCannotRefreshDueToLocksHeldError(opCtx, *extraInfo, retryCtx);
        }
        default:
            return CanRetry::NO;
    }
}

}  // namespace shard_role_loop
}  // namespace mongo
