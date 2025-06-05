/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/transaction/server_transactions_metrics.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
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
    return _totalStarted.loadRelaxed();
}

void ServerTransactionsMetrics::incrementTotalStarted() {
    _totalStarted.fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getTotalAborted() const {
    return _totalAborted.loadRelaxed();
}

void ServerTransactionsMetrics::incrementTotalAborted() {
    _totalAborted.fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getTotalCommitted() const {
    return _totalCommitted.loadRelaxed();
}

void ServerTransactionsMetrics::incrementTotalCommitted() {
    _totalCommitted.fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getTotalPrepared() const {
    return _totalPrepared.loadRelaxed();
}

void ServerTransactionsMetrics::incrementTotalPrepared() {
    _totalPrepared.fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getTotalPreparedThenCommitted() const {
    return _totalPreparedThenCommitted.loadRelaxed();
}

void ServerTransactionsMetrics::incrementTotalPreparedThenCommitted() {
    _totalPreparedThenCommitted.fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getTotalPreparedThenAborted() const {
    return _totalPreparedThenAborted.loadRelaxed();
}

void ServerTransactionsMetrics::incrementTotalPreparedThenAborted() {
    _totalPreparedThenAborted.fetchAndAddRelaxed(1);
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

void ServerTransactionsMetrics::updateLastTransaction(size_t operationCount,
                                                      size_t oplogOperationBytes,
                                                      BSONObj writeConcern) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
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
    stats->setTotalAborted(_totalAborted.loadRelaxed());
    stats->setTotalCommitted(_totalCommitted.loadRelaxed());
    stats->setTotalStarted(_totalStarted.loadRelaxed());
    stats->setTotalPrepared(_totalPrepared.loadRelaxed());
    stats->setTotalPreparedThenCommitted(_totalPreparedThenCommitted.loadRelaxed());
    stats->setTotalPreparedThenAborted(_totalPreparedThenAborted.loadRelaxed());
    stats->setCurrentPrepared(_currentPrepared.loadRelaxed());

    stdx::lock_guard<stdx::mutex> lg(_mutex);
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
        ServerTransactionsMetrics::get(opCtx)->updateStats(&stats, includeLastCommitted);
        return stats.toBSON();
    }
};
auto& transactionsSSS = *ServerStatusSectionBuilder<TransactionsSSS>("transactions").forShard();
}  // namespace

}  // namespace mongo
