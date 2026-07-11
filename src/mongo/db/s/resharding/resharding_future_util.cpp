// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_future_util.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/time_support.h"

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo::resharding {

bool shouldLogWriteBlockWarning(Atomic<long long>& lastWarningAt) {
    const auto now = Date_t::now();
    auto last = lastWarningAt.load();
    if (now <= Date_t::fromMillisSinceEpoch(last) + Minutes(1)) {
        return false;
    }
    // Under concurrent callers (e.g. multiple oplog appliers), let exactly one win the right to log
    // for this interval; the loser sees the updated timestamp and returns false.
    return lastWarningAt.compareAndSwap(&last, now.toMillisSinceEpoch());
}

std::vector<ExecutorFuture<void>> thenRunAllOn(const std::vector<SharedSemiFuture<void>>& futures,
                                               ExecutorPtr executor) {
    std::vector<ExecutorFuture<void>> result;
    result.reserve(futures.size());

    for (const auto& future : futures) {
        result.emplace_back(future.thenRunOn(executor));
    }

    return result;
}

ExecutorFuture<void> whenAllSucceedOn(const std::vector<SharedSemiFuture<void>>& futures,
                                      ExecutorPtr executor) {
    return !futures.empty() ? whenAllSucceed(thenRunAllOn(futures, executor)).thenRunOn(executor)
                            : ExecutorFuture(executor);
}

std::vector<Future<void>> runAllInlineUnsafe(const std::vector<SharedSemiFuture<void>>& futures) {
    std::vector<Future<void>> result;
    result.reserve(futures.size());

    for (const auto& future : futures) {
        result.emplace_back(future.unsafeToInlineFuture());
    }

    return result;
}

ExecutorFuture<void> cancelWhenAnyErrorThenQuiesce(
    const std::vector<SharedSemiFuture<void>>& futures,
    ExecutorPtr executor,
    CancellationSource cancelSource) {
    if (futures.empty()) {
        return ExecutorFuture(executor);
    }
    // Run all futures inline so that the onError callback is called even if that error was caused
    // by the executor shutting down. This causes the logic for whenAllSucceed, whenAll, and the
    // onError callback to potentially run on the threads of the setters of the promises
    // associated with the input futures. Since this logic is thread safe, not blocking, and does
    // not acquire additional resources, this is safe, but beware if making further changes to this
    // function.
    return whenAllSucceed(runAllInlineUnsafe(futures))
        .unsafeToInlineFuture()
        .onError([futures, cancelSource](Status originalError) mutable {
            cancelSource.cancel();

            return whenAll(runAllInlineUnsafe(futures))
                .ignoreValue()
                .unsafeToInlineFuture()
                .onCompletion([originalError](auto) { return originalError; });
        })
        .thenRunOn(executor);
}


}  // namespace mongo::resharding
