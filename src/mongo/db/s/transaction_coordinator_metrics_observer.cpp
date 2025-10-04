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
