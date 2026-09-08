// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/transaction/retryable_writes_stats.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction/transactions_stats_gen.h"
#include "mongo/util/decorable.h"

#include <algorithm>
#include <utility>

namespace mongo {
namespace {
const auto retryableWritesStatsDecoration =
    ServiceContext::declareDecoration<RetryableWritesStats>();
}  // namespace

RetryableWritesStats* RetryableWritesStats::get(ServiceContext* service) {
    return &retryableWritesStatsDecoration(service);
}

RetryableWritesStats* RetryableWritesStats::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void RetryableWritesStats::incrementRetryableCommandsCount() {
    _retryableCommandsCount.fetchAndAddRelaxed(1);
}

void RetryableWritesStats::incrementRetriedCommandsCount() {
    _retriedCommandsCount.fetchAndAddRelaxed(1);
}

void RetryableWritesStats::incrementRetriedStatementsCount() {
    _retriedStatementsCount.fetchAndAddRelaxed(1);
}

void RetryableWritesStats::incrementTransactionsCollectionWriteCount() {
    _transactionsCollectionWriteCount.fetchAndAddRelaxed(1);
}

std::vector<uint64_t> RetryableWritesStats::retriedWriteDelayPartitionsMillis() {
    return {
        1,
        10,
        100,
        500,
        1'000,
        5'000,
        10'000,
        30'000,
        60'000,
        180'000,
        300'000,
        600'000,
        1'200'000,
    };
    // The transaction record will be reaped after gTransactionRecordMinimumLifetimeMinutes,
    // 30 minutes by default (1'800'000 ms) so we cannot detect retries past this point.
}

void RetryableWritesStats::recordRetriedWriteDelay(Milliseconds delay) {
    // Record negative gaps (local-clock skew / same-instant) in the 0ms bucket.
    const auto delayMs = std::max(Milliseconds::zero(), delay).count();
    _retriedWriteDelayMillis.increment(static_cast<uint64_t>(delayMs));
}

// TODO(SERVER-134507): Add call to this to include latency histogram stats in serverStatus.
void RetryableWritesStats::appendRetriedWriteStats(BSONObjBuilder& bob) const {
    BSONArrayBuilder arr(bob.subarrayStart("retriedWritesDelayMillis"));
    for (auto&& bucket : _retriedWriteDelayMillis) {
        BSONObjBuilder(arr.subobjStart())
            .append("lowerBound", static_cast<long long>(bucket.lower ? *bucket.lower : 0))
            .append("count", static_cast<long long>(bucket.count));
    }
    arr.done();
}

void RetryableWritesStats::updateStats(TransactionsStats* stats) {
    stats->setRetryableCommandsCount(_retryableCommandsCount.loadRelaxed());
    stats->setRetriedCommandsCount(_retriedCommandsCount.loadRelaxed());
    stats->setRetriedStatementsCount(_retriedStatementsCount.loadRelaxed());
    stats->setTransactionsCollectionWriteCount(_transactionsCollectionWriteCount.loadRelaxed());
}

}  // namespace mongo
