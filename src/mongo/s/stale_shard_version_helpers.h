/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"

namespace mongo {

/**
 * Adds a log message with the given message. Simple helper to avoid defining the log component in a
 * header file.
 */
void logFailedRetryAttempt(StringData taskDescription, const DBException& ex);

/**
 * A retry loop which handles errors in ErrorCategory::StaleShardVersionError. When such an error is
 * encountered, the CatalogCache is marked for refresh and 'callback' is retried. When retried,
 * 'callback' will trigger a refresh of the CatalogCache and block until it's done when it next
 * consults the CatalogCache.
 */
template <typename F>
auto shardVersionRetry(OperationContext* opCtx,
                       CatalogCache* catalogCache,
                       NamespaceString nss,
                       StringData taskDescription,
                       F&& callbackFn) {
    size_t numAttempts = 0;
    auto logAndTestMaxRetries = [&numAttempts, taskDescription](auto& exception) {
        if (++numAttempts <= kMaxNumStaleVersionRetries) {
            logFailedRetryAttempt(taskDescription, exception);
            return true;
        }
        exception.addContext(str::stream()
                             << "Exceeded maximum number of " << kMaxNumStaleVersionRetries
                             << " retries attempting " << taskDescription);
        return false;
    };

    while (true) {
        catalogCache->setOperationShouldBlockBehindCatalogCacheRefresh(opCtx, numAttempts);

        try {
            return callbackFn();
        } catch (ExceptionFor<ErrorCodes::StaleDbVersion>& ex) {
            invariant(ex->getDb() == nss.db(),
                      str::stream() << "StaleDbVersion error on unexpected database. Expected "
                                    << nss.db() << ", received " << ex->getDb());

            // If the database version is stale, refresh its entry in the catalog cache.
            Grid::get(opCtx)->catalogCache()->onStaleDatabaseVersion(ex->getDb(),
                                                                     ex->getVersionWanted());

            if (!logAndTestMaxRetries(ex)) {
                throw;
            }
        } catch (ExceptionForCat<ErrorCategory::StaleShardVersionError>& e) {
            // If the exception provides a shardId, add it to the set of shards requiring a refresh.
            // If the cache currently considers the collection to be unsharded, this will trigger an
            // epoch refresh. If no shard is provided, then the epoch is stale and we must refresh.
            if (auto staleInfo = e.extraInfo<StaleConfigInfo>()) {
                invariant(staleInfo->getNss() == nss,
                          str::stream() << "StaleConfig error on unexpected namespace. Expected "
                                        << nss << ", received " << staleInfo->getNss());
                catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
                    nss, staleInfo->getVersionWanted(), staleInfo->getShardId());
            } else {
                catalogCache->invalidateCollectionEntry_LINEARIZABLE(nss);
            }
            if (!logAndTestMaxRetries(e)) {
                throw;
            }
        } catch (ExceptionFor<ErrorCodes::ShardInvalidatedForTargeting>& e) {
            if (!logAndTestMaxRetries(e)) {
                throw;
            }
        }
    }
}
}  // namespace mongo
