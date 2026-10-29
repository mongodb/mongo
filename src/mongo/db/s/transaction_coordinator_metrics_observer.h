// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/modules.h"


#pragma once

#include "mongo/db/client.h"
#include "mongo/db/s/server_transaction_coordinators_metrics.h"
#include "mongo/db/s/single_transaction_coordinator_stats.h"
#include "mongo/db/s/transaction_coordinator.h"
#include "mongo/db/s/transaction_coordinator_document_gen.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"

#include <boost/optional/optional.hpp>

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
     * Called when the transaction coordinator has recovered and continues the commit.
     */
    void onRecoveryFromFailover();

    /**
     * Updates relevant metrics when a transaction coordinator is about to start a new step.
     */
    void onStartStep(TransactionCoordinator::Step step,
                     TransactionCoordinator::Step previousStep,
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

    /**
     * Save information about the last client that interacted with this transaction.
     */
    void updateLastClientInfo(Client* client);

private:
    /**
     * Decrements the current active in 'step'.
     */
    void _decrementLastStep(ServerTransactionCoordinatorsMetrics*, TransactionCoordinator::Step);

    // Tracks metrics for a single commit coordination.
    SingleTransactionCoordinatorStats _singleTransactionCoordinatorStats;
};

}  // namespace mongo
