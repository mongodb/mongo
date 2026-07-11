// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/server_transaction_coordinators_metrics.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/s/transaction_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/util/decorable.h"

#include <utility>

namespace mongo {

auto& transactionCoordinatorSSS =
    *ServerStatusSectionBuilder<TransactionCoordinatorsSSS>("twoPhaseCommitCoordinator").forShard();

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
