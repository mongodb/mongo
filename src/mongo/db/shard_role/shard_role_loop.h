// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace shard_role_loop {

/**
 * Tracks the number of times a certain stale exception has occurred in the context of the current
 * retry loop. It is meant to put a limit to the number of retry attempts to prevent possible
 * runaway behavior.
 */

class [[MONGO_MOD_PUBLIC]] RetryContext {
public:
    RetryContext() = default;

    bool anyDbRefreshAttemptRemaining(const DatabaseName& dbName) const;
    bool anyShardVersionRefreshAttemptRemaining(const NamespaceString& nss) const;
    bool anyCatalogCacheRefreshAttemptRemaining(const NamespaceString& nss) const;

    void noteDbRefresh(const DatabaseName& dbName);
    void noteShardVersionRefresh(const NamespaceString& dbName);
    void noteCatalogCacheRefresh(const NamespaceString& dbName);

private:
    stdx::unordered_map<DatabaseName, int> _dbRefreshCountMap;
    stdx::unordered_map<NamespaceString, int> _shardVersionRefreshCountMap;
    stdx::unordered_map<NamespaceString, int> _catalogCacheRefreshCountMap;
};

enum class [[MONGO_MOD_PUBLIC]] CanRetry { NO, NO_BECAUSE_EXHAUSTED_RETRIES, YES };

/**
 * Determines if the provided Status 'error' corresponds to a stale shard error. If so, handles it
 * by recovering the metadata. Returns a value indicating whether the error was due to the shard
 * being stale and can now be retried.
 */
[[MONGO_MOD_PUBLIC]] CanRetry handleStaleError(OperationContext* opCtx,
                                               const Status& error,
                                               RetryContext& retryCtx);

/**
 * Runs the provided function 'fn', automatically handling and retrying stale shard metadata errors
 * in the event that the shard is stale and needs to recover its metadata.
 */
template <typename F>
[[MONGO_MOD_PUBLIC]] decltype(auto) withStaleShardRetry(OperationContext* opCtx, F&& fn) {
    RetryContext retryCtx;
    while (true) {
        try {
            return fn();
        } catch (DBException& ex) {
            const CanRetry canRetryStaleException =
                handleStaleError(opCtx, ex.toStatus(), retryCtx);
            if (canRetryStaleException == CanRetry::NO) {
                throw;
            }
            if (canRetryStaleException == CanRetry::NO_BECAUSE_EXHAUSTED_RETRIES) {
                ex.addContext("Exhausted maximum number of shard metadata recovery attempts");
                throw;
            }
        }
    }
}

}  // namespace shard_role_loop
}  // namespace mongo
