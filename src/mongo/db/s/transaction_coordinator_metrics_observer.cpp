// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/transaction_coordinator_metrics_observer.h"

#include "mongo/db/s/server_transaction_coordinators_metrics.h"
#include "mongo/db/s/transaction_coordinator.h"
#include "mongo/db/s/transaction_coordinator_structures.h"

#include <boost/optional/optional.hpp>

namespace mongo {

using CommitDecision = txn::CommitDecision;

void TransactionCoordinatorMetricsObserver::onCreate(
    ServerTransactionCoordinatorsMetrics* serverTransactionCoordinatorsMetrics,
    TickSource* tickSource,
    Date_t curWallClockTime) {

    //
    // Per transaction coordinator stats.
    //
    _singleTransactionCoordinatorStats.setCreateTime(tickSource->getTicks(), curWallClockTime);

    //
    // Server wide transaction coordinators metrics.
    //
    serverTransactionCoordinatorsMetrics->incrementTotalCreated();
}


void TransactionCoordinatorMetricsObserver::onRecoveryFromFailover() {
    _singleTransactionCoordinatorStats.setRecoveredFromFailover();
}

void TransactionCoordinatorMetricsObserver::onStartStep(
    TransactionCoordinator::Step step,
    TransactionCoordinator::Step previousStep,
    ServerTransactionCoordinatorsMetrics* serverTransactionCoordinatorsMetrics,
    TickSource* tickSource,
    Date_t curWallClockTime) {

    //
    // Per transaction coordinator stats.
    //
    _singleTransactionCoordinatorStats.setStepStartTime(
        step, tickSource->getTicks(), curWallClockTime);

    //
    // Server wide transaction coordinators metrics.
    //
    serverTransactionCoordinatorsMetrics->incrementCurrentInStep(step);
    if (step == TransactionCoordinator::Step::kWritingParticipantList) {
        serverTransactionCoordinatorsMetrics->incrementTotalStartedTwoPhaseCommit();
    } else {
        serverTransactionCoordinatorsMetrics->decrementCurrentInStep(previousStep);
    }
}

void TransactionCoordinatorMetricsObserver::onEnd(
    ServerTransactionCoordinatorsMetrics* serverTransactionCoordinatorsMetrics,
    TickSource* tickSource,
    Date_t curWallClockTime,
    TransactionCoordinator::Step step,
    const boost::optional<txn::CoordinatorCommitDecision>& decision) {

    //
    // Per transaction coordinator stats.
    //
    _singleTransactionCoordinatorStats.setEndTime(tickSource->getTicks(), curWallClockTime);

    //
    // Server wide transaction coordinators metrics.
    //
    if (decision) {
        switch (decision->getDecision()) {
            case CommitDecision::kCommit:
                serverTransactionCoordinatorsMetrics->incrementTotalSuccessfulTwoPhaseCommit();
                break;
            case CommitDecision::kAbort:
                serverTransactionCoordinatorsMetrics->incrementTotalAbortedTwoPhaseCommit();
                break;
        }
    }

    _decrementLastStep(serverTransactionCoordinatorsMetrics, step);
}

void TransactionCoordinatorMetricsObserver::_decrementLastStep(
    ServerTransactionCoordinatorsMetrics* serverTransactionCoordinatorsMetrics,
    TransactionCoordinator::Step step) {
    if (step != TransactionCoordinator::Step::kInactive) {
        serverTransactionCoordinatorsMetrics->decrementCurrentInStep(step);
    }
}

void TransactionCoordinatorMetricsObserver::updateLastClientInfo(Client* client) {
    _singleTransactionCoordinatorStats.updateLastClientInfo(client);
}
}  // namespace mongo
