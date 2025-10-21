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

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {
namespace shard_role_loop {

/**
 * Tracks the number of times a certain stale exception has occurred in the context of the current
 * retry loop. It is meant to put a limit to the number of retry attempts to prevent possible
 * runaway behavior.
 */

class RetryContext {
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

enum class CanRetry { NO, NO_BECAUSE_EXHAUSTED_RETRIES, YES };

/**
 * Determines if the provided Status 'error' corresponds to a stale shard error. If so, handles it
 * by recovering the metadata. Returns a value indicating whether the error was due to the shard
 * being stale and can now be retried.
 */
CanRetry handleStaleError(OperationContext* opCtx, const Status& error, RetryContext& retryCtx);


/**
 * Runs the provided function 'fn', automatically handling and retrying stale shard metadata errors
 * in the event that the shard is stale and needs to recover its metadata.
 */
template <typename F>
decltype(auto) withStaleShardRetry(OperationContext* opCtx, F&& fn) {
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
