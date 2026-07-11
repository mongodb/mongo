// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/single_transaction_coordinator_stats.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/s/transaction_coordinator.h"
#include "mongo/util/net/socket_utils.h"

namespace mongo {

void SingleTransactionCoordinatorStats::setCreateTime(TickSource::Tick curTick,
                                                      Date_t curWallClockTime) {
    invariant(!_times[createIndex()]);

    _times[createIndex()] = curTick;
    _wallClockTimes[createIndex()] = curWallClockTime;
}

void SingleTransactionCoordinatorStats::setEndTime(TickSource::Tick curTick,
                                                   Date_t curWallClockTime) {
    invariant(_times[createIndex()]);
    invariant(!_times[endIndex()]);

    _times[endIndex()] = curTick;
    _wallClockTimes[endIndex()] = curWallClockTime;
}

void SingleTransactionCoordinatorStats::setStepStartTime(TransactionCoordinator::Step step,
                                                         TickSource::Tick curTick,
                                                         Date_t curWallClockTime) {
    size_t stepIndex = static_cast<size_t>(step);
    invariant(!_times[stepIndex]);
    _times[stepIndex] = curTick;
    _wallClockTimes[stepIndex] = curWallClockTime;
}

void SingleTransactionCoordinatorStats::setRecoveredFromFailover() {
    _hasRecoveredFromFailover = true;
}

Microseconds SingleTransactionCoordinatorStats::getDurationSinceCreation(
    TickSource* tickSource, TickSource::Tick curTick) const {
    invariant(_times[createIndex()]);

    if (_times[endIndex()]) {
        return tickSource->ticksTo<Microseconds>(_times[endIndex()] - _times[createIndex()]);
    }
    return tickSource->ticksTo<Microseconds>(curTick - _times[createIndex()]);
}

Microseconds SingleTransactionCoordinatorStats::getTwoPhaseCommitDuration(
    TickSource* tickSource, TickSource::Tick curTick) const {
    const TickSource::Tick& writingParticipantListStartTime =
        _times[static_cast<size_t>(TransactionCoordinator::Step::kWritingParticipantList)];
    invariant(writingParticipantListStartTime);

    if (_times[endIndex()]) {
        return tickSource->ticksTo<Microseconds>(_times[endIndex()] -
                                                 writingParticipantListStartTime);
    }
    return tickSource->ticksTo<Microseconds>(curTick - writingParticipantListStartTime);
}

Microseconds SingleTransactionCoordinatorStats::getStepDuration(TransactionCoordinator::Step step,
                                                                TickSource* tickSource,
                                                                TickSource::Tick curTick) const {
    size_t index = static_cast<size_t>(step);
    const TickSource::Tick& stepStartTime = _times[index];

    if (step == TransactionCoordinator::Step::kWaitingForVotes && !stepStartTime) {
        // kWaitingForVotes start time can remain not set in a case of timeout.
        return Microseconds(0);
    }
    invariant(stepStartTime);

    if (_times[index + 1]) {
        return tickSource->ticksTo<Microseconds>(_times[index + 1] - stepStartTime);
    }

    if (_times[endIndex()]) {
        return tickSource->ticksTo<Microseconds>(_times[endIndex()] - stepStartTime);
    }

    return tickSource->ticksTo<Microseconds>(curTick - stepStartTime);
}

void SingleTransactionCoordinatorStats::reportMetrics(BSONObjBuilder& parent,
                                                      TickSource* tickSource,
                                                      TickSource::Tick curTick) const {
    BSONObjBuilder stepDurationsBuilder;

    invariant(_times[createIndex()]);
    parent.append("commitStartTime", _wallClockTimes[createIndex()]);
    parent.append("hasRecoveredFromFailover", _hasRecoveredFromFailover);

    if (hasTime(TransactionCoordinator::Step::kWritingParticipantList)) {
        const auto statValue = getStepDuration(
            TransactionCoordinator::Step::kWritingParticipantList, tickSource, curTick);
        stepDurationsBuilder.append("writingParticipantListMicros",
                                    durationCount<Microseconds>(statValue));

        const auto statValue2 = getTwoPhaseCommitDuration(tickSource, curTick);
        stepDurationsBuilder.append("totalCommitDurationMicros",
                                    durationCount<Microseconds>(statValue2));
    }

    if (hasTime(TransactionCoordinator::Step::kWaitingForVotes)) {
        const auto statValue =
            getStepDuration(TransactionCoordinator::Step::kWaitingForVotes, tickSource, curTick);
        stepDurationsBuilder.append("waitingForVotesMicros",
                                    durationCount<Microseconds>(statValue));
    }

    if (hasTime(TransactionCoordinator::Step::kWritingDecision)) {
        const auto statValue =
            getStepDuration(TransactionCoordinator::Step::kWritingDecision, tickSource, curTick);
        stepDurationsBuilder.append("writingDecisionMicros",
                                    durationCount<Microseconds>(statValue));
    }

    if (hasTime(TransactionCoordinator::Step::kWaitingForDecisionAcks)) {
        const auto statValue = getStepDuration(
            TransactionCoordinator::Step::kWaitingForDecisionAcks, tickSource, curTick);
        stepDurationsBuilder.append("waitingForDecisionAcksMicros",
                                    durationCount<Microseconds>(statValue));
    }

    if (hasTime(TransactionCoordinator::Step::kWritingEndOfTransaction)) {
        const auto statValue = getStepDuration(
            TransactionCoordinator::Step::kWritingEndOfTransaction, tickSource, curTick);
        stepDurationsBuilder.append("writingEndOfTransactionMicros",
                                    durationCount<Microseconds>(statValue));
    }

    if (hasTime(TransactionCoordinator::Step::kDeletingCoordinatorDoc)) {
        const auto statValue = getStepDuration(
            TransactionCoordinator::Step::kDeletingCoordinatorDoc, tickSource, curTick);
        stepDurationsBuilder.append("deletingCoordinatorDocMicros",
                                    durationCount<Microseconds>(statValue));
    }

    parent.append("stepDurations", stepDurationsBuilder.obj());
}

void SingleTransactionCoordinatorStats::reportLastClient(OperationContext* opCtx,
                                                         BSONObjBuilder& parent) const {
    parent.append("client", _lastClientInfo.clientHostAndPort);
    parent.append("host", prettyHostNameAndPort(opCtx->getClient()->getLocalPort()));
    parent.append("connectionId", _lastClientInfo.connectionId);
    parent.append("appName", _lastClientInfo.appName);
    parent.append("clientMetadata", _lastClientInfo.clientMetadata);
}
}  // namespace mongo
