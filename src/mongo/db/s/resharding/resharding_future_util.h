/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/base/status.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/s/primary_only_service_helpers/retrying_cancelable_operation_context_factory.h"
#include "mongo/db/s/primary_only_service_helpers/with_automatic_retry.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/out_of_line_executor.h"

#include <utility>
#include <vector>

namespace mongo {
namespace resharding {

using primary_only_service_helpers::kRetryabilityPredicateIncludeWriteConcernTimeout;
using primary_only_service_helpers::RetryabilityPredicate;
using primary_only_service_helpers::RetryingCancelableOperationContextFactory;

const auto kRetryabilityPredicateIncludeLockTimeoutAndWriteConcern = [](const Status& status) {
    return kRetryabilityPredicateIncludeWriteConcernTimeout(status) ||
        status == ErrorCodes::LockTimeout;
};

template <typename BodyCallable>
primary_only_service_helpers::WithAutomaticRetry<BodyCallable> WithAutomaticRetry(
    BodyCallable&& body) {
    return primary_only_service_helpers::WithAutomaticRetry<BodyCallable>(
        std::move(body), kRetryabilityPredicateIncludeWriteConcernTimeout);
}

/**
 * Converts a vector of SharedSemiFutures into a vector of ExecutorFutures.
 */
std::vector<ExecutorFuture<void>> thenRunAllOn(const std::vector<SharedSemiFuture<void>>& futures,
                                               ExecutorPtr executor);

/**
 * Given a vector of input futures, returns a future that becomes ready when either
 *
 *  (a) all of the input futures have become ready with success, or
 *  (b) one of the input futures has become ready with an error.
 *
 * This function returns an immediately ready future when the vector of input futures is empty.
 */
ExecutorFuture<void> whenAllSucceedOn(const std::vector<SharedSemiFuture<void>>& futures,
                                      ExecutorPtr executor);

/**
 * Given a vector of input futures, returns a future that becomes ready when all of the input
 * futures have become ready with success or failure.
 *
 * If one of the input futures becomes ready with an error, then the cancellation source is canceled
 * in an attempt to speed up the other input futures becoming ready. After all of the input futures
 * have become ready, the returned future becomes ready with the first error that had occurred.
 *
 * This function returns an immediately ready future when the vector of input futures is empty.
 */
ExecutorFuture<void> cancelWhenAnyErrorThenQuiesce(
    const std::vector<SharedSemiFuture<void>>& futures,
    ExecutorPtr executor,
    CancellationSource cancelSource);

}  // namespace resharding
}  // namespace mongo
