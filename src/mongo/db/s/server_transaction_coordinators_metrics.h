// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/modules.h"


#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/transaction_coordinator.h"
#include "mongo/db/s/transaction_coordinators_stats_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"

#include <cstdint>
#include <memory>

namespace mongo {

/**
 * Container for server-wide metrics about transaction two-phase commits.
 */
class ServerTransactionCoordinatorsMetrics {
    ServerTransactionCoordinatorsMetrics(const ServerTransactionCoordinatorsMetrics&) = delete;
    ServerTransactionCoordinatorsMetrics& operator=(const ServerTransactionCoordinatorsMetrics&) =
        delete;

public:
    ServerTransactionCoordinatorsMetrics();

    static ServerTransactionCoordinatorsMetrics* get(ServiceContext* service);
    static ServerTransactionCoordinatorsMetrics* get(OperationContext* opCtx);

    std::int64_t getTotalCreated();
    void incrementTotalCreated();

    std::int64_t getTotalStartedTwoPhaseCommit();
    void incrementTotalStartedTwoPhaseCommit();

    std::int64_t getCurrentInStep(TransactionCoordinator::Step step);
    void incrementCurrentInStep(TransactionCoordinator::Step step);
    void decrementCurrentInStep(TransactionCoordinator::Step step);

    std::int64_t getTotalAbortedTwoPhaseCommit();
    void incrementTotalAbortedTwoPhaseCommit();

    std::int64_t getTotalSuccessfulTwoPhaseCommit();
    void incrementTotalSuccessfulTwoPhaseCommit();

    /**
     * Appends the accumulated stats to a transaction coordinators stats object for reporting.
     */
    void updateStats(TransactionCoordinatorsStats* stats);

private:
    // The total number of transaction coordinators created on this process since the process's
    // inception.
    Atomic<std::int64_t> _totalCreated{0};

    // The total number of transaction coordinators on this process that started a two-phase commit
    // since the process's inception.
    Atomic<std::int64_t> _totalStartedTwoPhaseCommit{0};

    // The total number of transaction coordinators on this process that aborted a two-phase commit
    // since the process's inception.
    Atomic<std::int64_t> _totalAbortedTwoPhaseCommit{0};

    // The total number of transaction coordinators on this process that committed a two-phase
    // commit since the process's inception.
    Atomic<std::int64_t> _totalSuccessfulTwoPhaseCommit{0};

    // The number of transaction coordinators currently in the given step
    std::array<Atomic<std::int64_t>,
               static_cast<size_t>(TransactionCoordinator::Step::kLastStep) + 1>
        _totalInStep;
};

class TransactionCoordinatorsSSS final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override;
};

}  // namespace mongo
