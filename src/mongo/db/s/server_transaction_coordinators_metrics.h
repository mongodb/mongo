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

#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/transaction_coordinators_stats_gen.h"
#include "mongo/db/service_context.h"

namespace mongo {

/**
 * Container for server-wide metrics about transaction two-phase commits.
 */
class ServerTransactionCoordinatorsMetrics {
    ServerTransactionCoordinatorsMetrics(const ServerTransactionCoordinatorsMetrics&) = delete;
    ServerTransactionCoordinatorsMetrics& operator=(const ServerTransactionCoordinatorsMetrics&) =
        delete;

public:
    ServerTransactionCoordinatorsMetrics() = default;

    static ServerTransactionCoordinatorsMetrics* get(ServiceContext* service);
    static ServerTransactionCoordinatorsMetrics* get(OperationContext* opCtx);

    std::int64_t getTotalCreated();
    void incrementTotalCreated();

    std::int64_t getTotalStartedTwoPhaseCommit();
    void incrementTotalStartedTwoPhaseCommit();

    std::int64_t getCurrentWritingParticipantList();
    void incrementCurrentWritingParticipantList();
    void decrementCurrentWritingParticipantList();

    std::int64_t getCurrentWaitingForVotes();
    void incrementCurrentWaitingForVotes();
    void decrementCurrentWaitingForVotes();

    std::int64_t getCurrentWritingDecision();
    void incrementCurrentWritingDecision();
    void decrementCurrentWritingDecision();

    std::int64_t getCurrentWaitingForDecisionAcks();
    void incrementCurrentWaitingForDecisionAcks();
    void decrementCurrentWaitingForDecisionAcks();

    std::int64_t getCurrentDeletingCoordinatorDoc();
    void incrementCurrentDeletingCoordinatorDoc();
    void decrementCurrentDeletingCoordinatorDoc();

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
    AtomicWord<std::int64_t> _totalCreated{0};

    // The total number of transaction coordinators on this process that started a two-phase commit
    // since the process's inception.
    AtomicWord<std::int64_t> _totalStartedTwoPhaseCommit{0};

    // The total number of transaction coordinators on this process that aborted a two-phase commit
    // since the process's inception.
    AtomicWord<std::int64_t> _totalAbortedTwoPhaseCommit{0};

    // The total number of transaction coordinators on this process that committed a two-phase
    // commit since the process's inception.
    AtomicWord<std::int64_t> _totalSuccessfulTwoPhaseCommit{0};

    // The number of transaction coordinators currently in the "writing participant list" phase.
    AtomicWord<std::int64_t> _totalWritingParticipantList{0};

    // The number of transaction coordinators currently in the "waiting for votes" phase.
    AtomicWord<std::int64_t> _totalWaitingForVotes{0};

    // The number of transaction coordinators currently in the "writing decision" phase.
    AtomicWord<std::int64_t> _totalWritingDecision{0};

    // The number of transaction coordinators currently in the "waiting for decision acks" phase.
    AtomicWord<std::int64_t> _totalWaitingForDecisionAcks{0};

    // The number of transaction coordinators currently in the "deleting coordinator doc" phase.
    AtomicWord<std::int64_t> _totalDeletingCoordinatorDoc{0};
};

class TransactionCoordinatorsSSS final : public ServerStatusSection {
public:
    TransactionCoordinatorsSSS() : ServerStatusSection("twoPhaseCommitCoordinator") {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override;
};

}  // namespace mongo
