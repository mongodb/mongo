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

#pragma once

#include "mongo/db/s/server_transaction_coordinators_metrics.h"
#include "mongo/db/s/single_transaction_coordinator_stats.h"
#include "mongo/db/s/transaction_coordinator.h"

namespace mongo {

/**
 * Updates transaction coordinator metrics (per- two-phase commit metrics and server-wide two-phase
 * commit metrics) upon the appropriate event.
 */
class TransactionCoordinatorMetricsObserver {

public:
    /**
     * Updates relevant metrics when a transaction coordinator is created.
     */
    void onCreate(ServerTransactionCoordinatorsMetrics* serverTransactionCoordinatorMetrics,
                  TickSource* tickSource,
                  Date_t curWallClockTime);

    /**
     * Updates relevant metrics when a transaction coordinator is about to write the participant
     * list.
     */
    void onStartWritingParticipantList(
        ServerTransactionCoordinatorsMetrics* serverTransactionCoordinatorMetrics,
        TickSource* tickSource,
        Date_t curWallClockTime);

    /**
     * Updates relevant metrics when a transaction coordinator is about to send 'prepare' and start
     * waiting for votes (i.e., 'prepare' responses).
     */
    void onStartWaitingForVotes(
        ServerTransactionCoordinatorsMetrics* serverTransactionCoordinatorMetrics,
        TickSource* tickSource,
        Date_t curWallClockTime);

    /**
     * Updates relevant metrics when a transaction coordinator is about to write the decision.
     */
    void onStartWritingDecision(
        ServerTransactionCoordinatorsMetrics* serverTransactionCoordinatorMetrics,
        TickSource* tickSource,
        Date_t curWallClockTime);

    /**
     * Updates relevant metrics when a transaction coordinator is about to send the decision to
     * participants and start waiting for acknowledgements.
     */
    void onStartWaitingForDecisionAcks(
        ServerTransactionCoordinatorsMetrics* serverTransactionCoordinatorMetrics,
        TickSource* tickSource,
        Date_t curWallClockTime);

    /**
     * Updates relevant metrics when a transaction coordinator is about to delete its durable state.
     */
    void onStartDeletingCoordinatorDoc(
        ServerTransactionCoordinatorsMetrics* serverTransactionCoordinatorMetrics,
        TickSource* tickSource,
        Date_t curWallClockTime);

    /**
     * Updates relevant metrics when a transaction coordinator is destroyed.
     *
     * The 'lastStep' parameter is needed because, unlike for the other state transitions, the
     * coordinator can transition to the end state from any other state, for example on stepdown.
     */
    void onEnd(ServerTransactionCoordinatorsMetrics* serverTransactionCoordinatorMetrics,
               TickSource* tickSource,
               Date_t curWallClockTime,
               TransactionCoordinator::Step lastStep,
               const boost::optional<txn::CoordinatorCommitDecision>& decision);

    /**
     * Returns a read-only reference to the SingleTransactionCoordinatorStats object stored in this
     * TransactionCoordinatorMetricsObserver instance.
     */
    const SingleTransactionCoordinatorStats& getSingleTransactionCoordinatorStats() const {
        return _singleTransactionCoordinatorStats;
    }

private:
    /**
     * Decrements the current active in 'step'.
     */
    void _decrementLastStep(ServerTransactionCoordinatorsMetrics*, TransactionCoordinator::Step);

    // Tracks metrics for a single commit coordination.
    SingleTransactionCoordinatorStats _singleTransactionCoordinatorStats;
};

}  // namespace mongo
