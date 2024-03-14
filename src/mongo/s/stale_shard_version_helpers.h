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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/service_context.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future_util.h"
#include "mongo/util/out_of_line_executor.h"

namespace mongo {

namespace shard_version_retry {

void checkErrorStatusAndMaxRetries(const Status& status,
                                   const NamespaceString& nss,
                                   CatalogCache* catalogCache,
                                   StringData taskDescription,
                                   size_t numAttempts);

/**
 * Performs necessary cache invalidations based on the error status.
 * Throws if it encountered an unexpected status or numAttempts exceeded maximum amount.
 */
template <typename T>
void checkErrorStatusAndMaxRetries(const StatusWith<T>& status,
                                   const NamespaceString& nss,
                                   CatalogCache* catalogCache,
                                   StringData taskDescription,
                                   size_t numAttempts) {
    return checkErrorStatusAndMaxRetries(
        status.getStatus(), nss, catalogCache, taskDescription, numAttempts);
}

}  // namespace shard_version_retry

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
                       const F& callbackFn) {
    size_t numAttempts = 0;

    while (true) {
        try {
            return callbackFn();
        } catch (const DBException& ex) {
            shard_version_retry::checkErrorStatusAndMaxRetries(
                ex.toStatus(), nss, catalogCache, taskDescription, ++numAttempts);
        }
    }
}

/**
 * Async loop for retrying stale database/shard version a finite number of times. callbackFn should
 * accept OperationContext* as an argument.
 *
 * Note: Currently only supports void return type for callbackFn.
 */
template <typename Callable>
auto shardVersionRetry(ServiceContext* service,
                       NamespaceString nss,
                       CatalogCache* catalogCache,
                       StringData taskDescription,
                       ExecutorPtr executor,
                       CancellationToken cancelToken,
                       Callable callbackFn) {
    auto numAttempts = std::make_shared<size_t>(0);

    auto body = [service,
                 catalogCache,
                 taskDescription,
                 numAttempts,
                 _callbackFn = std::move(callbackFn),
                 executor,
                 cancelToken] {
        boost::optional<ThreadClient> threadClient;
        if (!haveClient()) {
            threadClient.emplace(taskDescription, service->getService());
        }

        CancelableOperationContextFactory opCtxFactory(cancelToken, executor);
        auto cancelableOpCtx = opCtxFactory.makeOperationContext(&cc());
        auto opCtx = cancelableOpCtx.get();

        return _callbackFn(opCtx);
    };

    using ResultType = std::invoke_result_t<Callable, OperationContext*>;

    return AsyncTry<decltype(body)>(std::move(body))
        .until([numAttempts, _nss = std::move(nss), catalogCache, taskDescription](
                   const StatusOrStatusWith<ResultType>& statusOrStatusWith) {
            if (statusOrStatusWith.isOK()) {
                return true;
            }

            shard_version_retry::checkErrorStatusAndMaxRetries(
                statusOrStatusWith, _nss, catalogCache, taskDescription, ++(*numAttempts));
            return false;
        })
        .on(std::move(executor), cancelToken);
}

}  // namespace mongo
