// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/transaction/retryable_writes_stats.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction/transactions_stats_gen.h"
#include "mongo/util/decorable.h"

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

void RetryableWritesStats::incrementRetriedCommandsCount() {
    _retriedCommandsCount.fetchAndAddRelaxed(1);
}

void RetryableWritesStats::incrementRetriedStatementsCount() {
    _retriedStatementsCount.fetchAndAddRelaxed(1);
}

void RetryableWritesStats::incrementTransactionsCollectionWriteCount() {
    _transactionsCollectionWriteCount.fetchAndAddRelaxed(1);
}

void RetryableWritesStats::updateStats(TransactionsStats* stats) {
    stats->setRetriedCommandsCount(_retriedCommandsCount.loadRelaxed());
    stats->setRetriedStatementsCount(_retriedStatementsCount.loadRelaxed());
    stats->setTransactionsCollectionWriteCount(_transactionsCollectionWriteCount.loadRelaxed());
}

}  // namespace mongo
