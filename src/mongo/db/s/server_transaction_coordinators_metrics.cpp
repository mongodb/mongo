/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <utility>

#include "mongo/db/operation_context.h"
#include "mongo/db/s/server_transaction_coordinators_metrics.h"
#include "mongo/db/s/transaction_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/util/decorable.h"

namespace mongo {

auto& transactionCoordinatorSSS =
    *ServerStatusSectionBuilder<TransactionCoordinatorsSSS>("twoPhaseCommitCoordinator");

namespace {
const auto ServerTransactionCoordinatorsMetricsDecoration =
    ServiceContext::declareDecoration<ServerTransactionCoordinatorsMetrics>();
}  // namespace

ServerTransactionCoordinatorsMetrics* ServerTransactionCoordinatorsMetrics::get(
    ServiceContext* service) {
    return &ServerTransactionCoordinatorsMetricsDecoration(service);
}

ServerTransactionCoordinatorsMetrics* ServerTransactionCoordinatorsMetrics::get(
    OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

ServerTransactionCoordinatorsMetrics::ServerTransactionCoordinatorsMetrics() {
    for (auto& totalInStep : _totalInStep) {
        totalInStep.store(0);
    }
}

std::int64_t ServerTransactionCoordinatorsMetrics::getTotalCreated() {
    return _totalCreated.load();
}
void ServerTransactionCoordinatorsMetrics::incrementTotalCreated() {
    _totalCreated.fetchAndAdd(1);
}

std::int64_t ServerTransactionCoordinatorsMetrics::getTotalStartedTwoPhaseCommit() {
    return _totalStartedTwoPhaseCommit.load();
}
void ServerTransactionCoordinatorsMetrics::incrementTotalStartedTwoPhaseCommit() {
    _totalStartedTwoPhaseCommit.fetchAndAdd(1);
}

std::int64_t ServerTransactionCoordinatorsMetrics::getTotalAbortedTwoPhaseCommit() {
    return _totalAbortedTwoPhaseCommit.load();
}

void ServerTransactionCoordinatorsMetrics::incrementTotalAbortedTwoPhaseCommit() {
    _totalAbortedTwoPhaseCommit.fetchAndAdd(1);
}

std::int64_t ServerTransactionCoordinatorsMetrics::getTotalSuccessfulTwoPhaseCommit() {
    return _totalSuccessfulTwoPhaseCommit.load();
}

void ServerTransactionCoordinatorsMetrics::incrementTotalSuccessfulTwoPhaseCommit() {
    _totalSuccessfulTwoPhaseCommit.fetchAndAdd(1);
}

std::int64_t ServerTransactionCoordinatorsMetrics::getCurrentInStep(
    TransactionCoordinator::Step step) {
    return _totalInStep[static_cast<size_t>(step)].load();
}
void ServerTransactionCoordinatorsMetrics::incrementCurrentInStep(
    TransactionCoordinator::Step step) {
    _totalInStep[static_cast<size_t>(step)].fetchAndAdd(1);
}
void ServerTransactionCoordinatorsMetrics::decrementCurrentInStep(
    TransactionCoordinator::Step step) {
    _totalInStep[static_cast<size_t>(step)].fetchAndSubtract(1);
}

void ServerTransactionCoordinatorsMetrics::updateStats(TransactionCoordinatorsStats* stats) {
    stats->setTotalCreated(_totalCreated.load());
    stats->setTotalStartedTwoPhaseCommit(_totalStartedTwoPhaseCommit.load());
    stats->setTotalAbortedTwoPhaseCommit(_totalAbortedTwoPhaseCommit.load());
    stats->setTotalCommittedTwoPhaseCommit(_totalSuccessfulTwoPhaseCommit.load());

    CurrentInSteps currentInSteps;
    currentInSteps.setWritingParticipantList(
        getCurrentInStep(TransactionCoordinator::Step::kWritingParticipantList));
    currentInSteps.setWaitingForVotes(
        getCurrentInStep(TransactionCoordinator::Step::kWaitingForVotes));
    currentInSteps.setWritingDecision(
        getCurrentInStep(TransactionCoordinator::Step::kWritingDecision));
    currentInSteps.setWaitingForDecisionAcks(
        getCurrentInStep(TransactionCoordinator::Step::kWaitingForDecisionAcks));
    currentInSteps.setWritingEndOfTransaction(
        getCurrentInStep(TransactionCoordinator::Step::kWritingEndOfTransaction));
    currentInSteps.setDeletingCoordinatorDoc(
        getCurrentInStep(TransactionCoordinator::Step::kDeletingCoordinatorDoc));
    stats->setCurrentInSteps(currentInSteps);
}

BSONObj TransactionCoordinatorsSSS::generateSection(OperationContext* opCtx,
                                                    const BSONElement& configElement) const {
    TransactionCoordinatorsStats stats;
    ServerTransactionCoordinatorsMetrics::get(opCtx)->updateStats(&stats);
    return stats.toBSON();
}

}  // namespace mongo
