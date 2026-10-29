// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/transaction/server_transactions_metrics.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction/reclaimed_prepared_txn_tracker.h"
#include "mongo/db/transaction/retryable_writes_stats.h"
#include "mongo/db/transaction/transactions_stats_gen.h"
#include "mongo/util/decorable.h"

#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {
const auto ServerTransactionsMetricsDecoration =
    ServiceContext::declareDecoration<ServerTransactionsMetrics>();
}  // namespace

ServerTransactionsMetrics* ServerTransactionsMetrics::get(ServiceContext* service) {
    return &ServerTransactionsMetricsDecoration(service);
}

ServerTransactionsMetrics* ServerTransactionsMetrics::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

unsigned long long ServerTransactionsMetrics::getCurrentActive() const {
    return _currentActive.loadRelaxed();
}

void ServerTransactionsMetrics::decrementCurrentActive() {
    _currentActive.fetchAndSubtractRelaxed(1);
}

void ServerTransactionsMetrics::incrementCurrentActive() {
    _currentActive.fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getCurrentInactive() const {
    return _currentInactive.loadRelaxed();
}

void ServerTransactionsMetrics::decrementCurrentInactive() {
    _currentInactive.fetchAndSubtractRelaxed(1);
}

void ServerTransactionsMetrics::incrementCurrentInactive() {
    _currentInactive.fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getCurrentOpen() const {
    return _currentOpen.loadRelaxed();
}

void ServerTransactionsMetrics::decrementCurrentOpen() {
    _currentOpen.fetchAndSubtractRelaxed(1);
}

void ServerTransactionsMetrics::incrementCurrentOpen() {
    _currentOpen.fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getTotalStarted() const {
    return _totalStartedInternal.loadRelaxed() + _totalStartedExternal.loadRelaxed();
}

unsigned long long ServerTransactionsMetrics::getTotalStartedInternal() const {
    return _totalStartedInternal.loadRelaxed();
}

unsigned long long ServerTransactionsMetrics::getTotalStartedExternal() const {
    return _totalStartedExternal.loadRelaxed();
}

void ServerTransactionsMetrics::incrementTotalStarted(bool isServerInitiated) {
    (isServerInitiated ? _totalStartedInternal : _totalStartedExternal).fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getTotalAborted() const {
    return _totalAbortedInternal.loadRelaxed() + _totalAbortedExternal.loadRelaxed();
}

unsigned long long ServerTransactionsMetrics::getTotalAbortedInternal() const {
    return _totalAbortedInternal.loadRelaxed();
}

unsigned long long ServerTransactionsMetrics::getTotalAbortedExternal() const {
    return _totalAbortedExternal.loadRelaxed();
}

void ServerTransactionsMetrics::incrementTotalAborted(bool isServerInitiated) {
    (isServerInitiated ? _totalAbortedInternal : _totalAbortedExternal).fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getTotalCommitted() const {
    return _totalCommittedInternal.loadRelaxed() + _totalCommittedExternal.loadRelaxed();
}

unsigned long long ServerTransactionsMetrics::getTotalCommittedInternal() const {
    return _totalCommittedInternal.loadRelaxed();
}

unsigned long long ServerTransactionsMetrics::getTotalCommittedExternal() const {
    return _totalCommittedExternal.loadRelaxed();
}

void ServerTransactionsMetrics::incrementTotalCommitted(bool isServerInitiated) {
    (isServerInitiated ? _totalCommittedInternal : _totalCommittedExternal).fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getTotalPrepared() const {
    return _totalPreparedInternal.loadRelaxed() + _totalPreparedExternal.loadRelaxed();
}

unsigned long long ServerTransactionsMetrics::getTotalPreparedInternal() const {
    return _totalPreparedInternal.loadRelaxed();
}

unsigned long long ServerTransactionsMetrics::getTotalPreparedExternal() const {
    return _totalPreparedExternal.loadRelaxed();
}

void ServerTransactionsMetrics::incrementTotalPrepared(bool isServerInitiated) {
    (isServerInitiated ? _totalPreparedInternal : _totalPreparedExternal).fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getTotalPreparedThenCommitted() const {
    return _totalPreparedThenCommittedInternal.loadRelaxed() +
        _totalPreparedThenCommittedExternal.loadRelaxed();
}

unsigned long long ServerTransactionsMetrics::getTotalPreparedThenCommittedInternal() const {
    return _totalPreparedThenCommittedInternal.loadRelaxed();
}

unsigned long long ServerTransactionsMetrics::getTotalPreparedThenCommittedExternal() const {
    return _totalPreparedThenCommittedExternal.loadRelaxed();
}

void ServerTransactionsMetrics::incrementTotalPreparedThenCommitted(bool isServerInitiated) {
    (isServerInitiated ? _totalPreparedThenCommittedInternal : _totalPreparedThenCommittedExternal)
        .fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getTotalPreparedThenAborted() const {
    return _totalPreparedThenAbortedInternal.loadRelaxed() +
        _totalPreparedThenAbortedExternal.loadRelaxed();
}

unsigned long long ServerTransactionsMetrics::getTotalPreparedThenAbortedInternal() const {
    return _totalPreparedThenAbortedInternal.loadRelaxed();
}

unsigned long long ServerTransactionsMetrics::getTotalPreparedThenAbortedExternal() const {
    return _totalPreparedThenAbortedExternal.loadRelaxed();
}

void ServerTransactionsMetrics::incrementTotalPreparedThenAborted(bool isServerInitiated) {
    (isServerInitiated ? _totalPreparedThenAbortedInternal : _totalPreparedThenAbortedExternal)
        .fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getCurrentPrepared() const {
    return _currentPrepared.loadRelaxed();
}

void ServerTransactionsMetrics::incrementCurrentPrepared() {
    _currentPrepared.fetchAndAddRelaxed(1);
}

void ServerTransactionsMetrics::decrementCurrentPrepared() {
    _currentPrepared.fetchAndSubtractRelaxed(1);
}

void ServerTransactionsMetrics::incrementReclaimedPreparedTxnsCommitted() {
    _reclaimedPreparedTxnsCommitted.fetchAndAddRelaxed(1);
}

long long ServerTransactionsMetrics::getReclaimedPreparedTxnsCommitted() const {
    return _reclaimedPreparedTxnsCommitted.loadRelaxed();
}

void ServerTransactionsMetrics::incrementReclaimedPreparedTxnsAborted() {
    _reclaimedPreparedTxnsAborted.fetchAndAddRelaxed(1);
}

long long ServerTransactionsMetrics::getReclaimedPreparedTxnsAborted() const {
    return _reclaimedPreparedTxnsAborted.loadRelaxed();
}

void ServerTransactionsMetrics::updateLastTransaction(size_t operationCount,
                                                      size_t oplogOperationBytes,
                                                      BSONObj writeConcern) {
    std::lock_guard<std::mutex> lg(_mutex);
    if (!_lastCommittedTransaction) {
        _lastCommittedTransaction = LastCommittedTransaction();
    }
    _lastCommittedTransaction->setOperationCount(operationCount);
    _lastCommittedTransaction->setOplogOperationBytes(oplogOperationBytes);
    _lastCommittedTransaction->setWriteConcern(std::move(writeConcern));
}

void ServerTransactionsMetrics::updateStats(TransactionsStats* stats, bool includeLastCommitted) {
    stats->setCurrentActive(_currentActive.loadRelaxed());
    stats->setCurrentInactive(_currentInactive.loadRelaxed());
    stats->setCurrentOpen(_currentOpen.loadRelaxed());
    // Load each counter once so the aggregate always equals the sum of its split fields.
    const auto abortedInternal = _totalAbortedInternal.loadRelaxed();
    const auto abortedExternal = _totalAbortedExternal.loadRelaxed();
    stats->setTotalAborted(abortedInternal + abortedExternal);
    stats->setTotalAbortedInternal(abortedInternal);
    stats->setTotalAbortedExternal(abortedExternal);

    const auto committedInternal = _totalCommittedInternal.loadRelaxed();
    const auto committedExternal = _totalCommittedExternal.loadRelaxed();
    stats->setTotalCommitted(committedInternal + committedExternal);
    stats->setTotalCommittedInternal(committedInternal);
    stats->setTotalCommittedExternal(committedExternal);

    const auto startedInternal = _totalStartedInternal.loadRelaxed();
    const auto startedExternal = _totalStartedExternal.loadRelaxed();
    stats->setTotalStarted(startedInternal + startedExternal);
    stats->setTotalStartedInternal(startedInternal);
    stats->setTotalStartedExternal(startedExternal);

    const auto preparedInternal = _totalPreparedInternal.loadRelaxed();
    const auto preparedExternal = _totalPreparedExternal.loadRelaxed();
    stats->setTotalPrepared(preparedInternal + preparedExternal);
    stats->setTotalPreparedInternal(preparedInternal);
    stats->setTotalPreparedExternal(preparedExternal);

    const auto preparedThenCommittedInternal = _totalPreparedThenCommittedInternal.loadRelaxed();
    const auto preparedThenCommittedExternal = _totalPreparedThenCommittedExternal.loadRelaxed();
    stats->setTotalPreparedThenCommitted(preparedThenCommittedInternal +
                                         preparedThenCommittedExternal);
    stats->setTotalPreparedThenCommittedInternal(preparedThenCommittedInternal);
    stats->setTotalPreparedThenCommittedExternal(preparedThenCommittedExternal);

    const auto preparedThenAbortedInternal = _totalPreparedThenAbortedInternal.loadRelaxed();
    const auto preparedThenAbortedExternal = _totalPreparedThenAbortedExternal.loadRelaxed();
    stats->setTotalPreparedThenAborted(preparedThenAbortedInternal + preparedThenAbortedExternal);
    stats->setTotalPreparedThenAbortedInternal(preparedThenAbortedInternal);
    stats->setTotalPreparedThenAbortedExternal(preparedThenAbortedExternal);

    stats->setCurrentPrepared(_currentPrepared.loadRelaxed());

    std::lock_guard<std::mutex> lg(_mutex);
    if (_lastCommittedTransaction && includeLastCommitted) {
        stats->setLastCommittedTransaction(*_lastCommittedTransaction);
    }
}

namespace {
class TransactionsSSS : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    ~TransactionsSSS() override = default;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        TransactionsStats stats;

        bool includeLastCommitted = true;
        if (configElement.type() == BSONType::object) {
            includeLastCommitted = configElement.Obj()["includeLastCommitted"].trueValue();
        }

        // Retryable writes and multi-document transactions metrics are both included in the same
        // serverStatus section because both utilize similar internal machinery for tracking their
        // lifecycle within a session. Both are assigned transaction numbers, and so both are often
        // referred to as “transactions”.
        RetryableWritesStats::get(opCtx)->updateStats(&stats);
        auto* serverTxnMetrics = ServerTransactionsMetrics::get(opCtx);
        serverTxnMetrics->updateStats(&stats, includeLastCommitted);

        auto* tracker = ReclaimedPreparedTxnTracker::get(opCtx);
        auto committed = serverTxnMetrics->getReclaimedPreparedTxnsCommitted();
        auto aborted = serverTxnMetrics->getReclaimedPreparedTxnsAborted();
        auto remaining = tracker->getNumReclaimedPreparedTxnsRemaining();
        if (committed + aborted + remaining > 0) {
            PreciseCheckpointRecoveryStats recoveryStats;
            recoveryStats.setNumReclaimedPreparedTxnsExitedPrepare(committed + aborted);
            recoveryStats.setNumReclaimedPreparedTxnsCommitted(committed);
            recoveryStats.setNumReclaimedPreparedTxnsAborted(aborted);
            recoveryStats.setNumReclaimedPreparedTxnsRemaining(remaining);
            recoveryStats.setRecoveryDurationMicros(tracker->getRecoveryDurationMicros());
            stats.setPreciseCheckpointRecovery(recoveryStats);
        }

        // Append the retry-delay latency histogram alongside the IDL-generated TransactionsStats
        // fields. A variable-length histogram isn't expressible as a fixed strict IDL field, so
        // it is added as an extra field on the outer "transactions" BSON object.
        auto retryStats = RetryableWritesStats::get(opCtx);
        BSONObjBuilder result;
        result.appendElements(stats.toBSON());
        retryStats->appendRetriedWriteStats(result);

        return result.obj();
    }
};
auto& transactionsSSS = *ServerStatusSectionBuilder<TransactionsSSS>("transactions").forShard();
}  // namespace

}  // namespace mongo
