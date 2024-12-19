/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <memory>

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"
#include "mongo/util/future_util.h"

namespace mongo::async_rpc {

/**
 * Template to combine futures using a future processing callable.
 */
template <typename ResultType, typename FutureType, typename ProcessSWCallable>
Future<ResultType> processMultipleFutures(std::vector<ExecutorFuture<FutureType>>&& futures,
                                          ProcessSWCallable&& processStatusWith) {
    auto [resultPromise, resultFuture] = makePromiseFuture<ResultType>();

    // Dependent on caller to synchronize sharedPromise access in processStatusWith.
    std::shared_ptr<Promise<ResultType>> sharedPromise =
        std::make_shared<Promise<ResultType>>(std::move(resultPromise));

    for (size_t i = 0; i < futures.size(); ++i) {
        std::move(futures[i])
            .unsafeToInlineFuture()  // always process the result, even if an executor is rejecting
                                     // work
            .getAsync(
                [index = i, sharedPromise, processStatusWith](StatusOrStatusWith<FutureType> sw) {
                    processStatusWith(sw, sharedPromise, index);
                });
    }
    return std::move(resultFuture);
}

/**
 * Given a vector of input Futures, returns a Future which holds the value
 * of the first of those futures to resolve with a status, value, and index that
 * satisfies the conditions in the ConditionCallable Callable.
 */
template <typename ResultType, typename ConditionCallable>
Future<ResultType> whenAnyThat(std::vector<ExecutorFuture<ResultType>>&& futures,
                               ConditionCallable&& shouldAccept) {
    std::shared_ptr<AtomicWord<bool>> done = std::make_shared<AtomicWord<bool>>(false);
    invariant(futures.size() > 0);

    auto processSW = [shouldAccept, done](StatusOrStatusWith<ResultType> value,
                                          std::shared_ptr<Promise<ResultType>> promise,
                                          size_t index) {
        if (shouldAccept(value, index)) {
            // If this is the first input future to complete and satisfy the
            // shouldAccept condition, change done to true and set the value on the
            // promise.
            if (!done->swap(true)) {
                promise->setFrom(std::move(value));
            }
        }
    };

    return processMultipleFutures<ResultType>(std::move(futures), std::move(processSW));
}

void logErrorDetails(int responsesLeft, Status errorStatus);

/**
 * Given a vector of input Futures and a processResponse callable, processes the responses
 * from each of the futures and pushes the results onto a vector. Cancels early on error
 * status, but waits until other futures resolve. Caller must manually create a
 * CancellationSource wrapping the sendCommand cancellation token.
 */
template <typename SingleResult, typename FutureType, typename ProcessResponseCallable>
Future<std::vector<SingleResult>> getAllResponsesOrFirstErrorWithCancellation(
    std::vector<ExecutorFuture<FutureType>>&& futures,
    CancellationSource cancelSource,
    ProcessResponseCallable&& processResponse) {

    struct SharedUtil {
        SharedUtil(int responsesLeft, CancellationSource cancelSource)
            : responsesLeft(responsesLeft), source(cancelSource) {}
        stdx::mutex mutex;
        int responsesLeft;
        StatusWith<std::vector<SingleResult>> results =
            StatusWith<std::vector<SingleResult>>(std::vector<SingleResult>());
        CancellationSource source;
    };

    auto sharedUtil = std::make_shared<SharedUtil>(futures.size(), cancelSource);
    auto processWrapper = [sharedUtil, processResponse](
                              StatusOrStatusWith<FutureType> sw,
                              std::shared_ptr<Promise<std::vector<SingleResult>>> sharedPromise,
                              size_t index) {
        if (!sw.isOK()) {
            sharedUtil->source.cancel();
            stdx::lock_guard<stdx::mutex> lk(sharedUtil->mutex);
            // TODO(SERVER-98556): Debug statement for the purpose of helping with diagnosing BFs.
            logErrorDetails(sharedUtil->responsesLeft, sw.getStatus());
            if (sharedUtil->results.getStatus() == Status::OK()) {
                sharedUtil->results = sw.getStatus();
            }
            // Need to wait for all responses to protect against pending work after promise is
            // fulfilled.
            if (--sharedUtil->responsesLeft == 0) {
                sharedPromise->setFrom(sharedUtil->results);
            }
            return;
        }

        auto reply = sw.getValue();
        auto response = processResponse(reply, index);

        stdx::lock_guard<stdx::mutex> lk(sharedUtil->mutex);
        if (sharedUtil->results.getStatus() == Status::OK()) {
            sharedUtil->results.getValue().push_back(response);
        }
        if (--sharedUtil->responsesLeft == 0) {
            sharedPromise->setFrom(sharedUtil->results);
        }
    };

    return processMultipleFutures<std::vector<SingleResult>>(std::move(futures),
                                                             std::move(processWrapper));
}

}  // namespace mongo::async_rpc
