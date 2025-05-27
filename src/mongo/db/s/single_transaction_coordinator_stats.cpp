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
