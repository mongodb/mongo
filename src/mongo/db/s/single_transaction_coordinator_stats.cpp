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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/s/single_transaction_coordinator_stats.h"
#include "mongo/util/net/socket_utils.h"

namespace mongo {

void SingleTransactionCoordinatorStats::setCreateTime(TickSource::Tick curTick,
                                                      Date_t curWallClockTime) {
    invariant(!_createTime);

    _createTime = curTick;
    _createWallClockTime = curWallClockTime;
}

void SingleTransactionCoordinatorStats::setEndTime(TickSource::Tick curTick,
                                                   Date_t curWallClockTime) {
    invariant(_createTime);
    invariant(!_endTime);

    _endTime = curTick;
    _endWallClockTime = curWallClockTime;
}

void SingleTransactionCoordinatorStats::setWritingParticipantListStartTime(
    TickSource::Tick curTick, Date_t curWallClockTime) {
    invariant(_createTime);
    invariant(!_writingParticipantListStartTime);

    _writingParticipantListStartTime = curTick;
    _writingParticipantListStartWallClockTime = curWallClockTime;
}

void SingleTransactionCoordinatorStats::setWaitingForVotesStartTime(TickSource::Tick curTick,
                                                                    Date_t curWallClockTime) {
    invariant(_writingParticipantListStartTime);
    invariant(!_waitingForVotesStartTime);

    _waitingForVotesStartTime = curTick;
    _waitingForVotesStartWallClockTime = curWallClockTime;
}

void SingleTransactionCoordinatorStats::setWritingDecisionStartTime(TickSource::Tick curTick,
                                                                    Date_t curWallClockTime) {
    invariant(_waitingForVotesStartTime);
    invariant(!_writingDecisionStartTime);

    _writingDecisionStartTime = curTick;
    _writingDecisionStartWallClockTime = curWallClockTime;
}

void SingleTransactionCoordinatorStats::setWaitingForDecisionAcksStartTime(
    TickSource::Tick curTick, Date_t curWallClockTime) {
    invariant(_writingDecisionStartTime);
    invariant(!_waitingForDecisionAcksStartTime);

    _waitingForDecisionAcksStartTime = curTick;
    _waitingForDecisionAcksStartWallClockTime = curWallClockTime;
}

void SingleTransactionCoordinatorStats::setDeletingCoordinatorDocStartTime(
    TickSource::Tick curTick, Date_t curWallClockTime) {
    invariant(!_deletingCoordinatorDocStartTime);

    _deletingCoordinatorDocStartTime = curTick;
    _deletingCoordinatorDocStartWallClockTime = curWallClockTime;
}

Microseconds SingleTransactionCoordinatorStats::getDurationSinceCreation(
    TickSource* tickSource, TickSource::Tick curTick) const {
    invariant(_createTime);

    if (_endTime) {
        return tickSource->ticksTo<Microseconds>(_endTime - _createTime);
    }
    return tickSource->ticksTo<Microseconds>(curTick - _createTime);
}

Microseconds SingleTransactionCoordinatorStats::getTwoPhaseCommitDuration(
    TickSource* tickSource, TickSource::Tick curTick) const {
    invariant(_writingParticipantListStartTime);

    if (_endTime) {
        return tickSource->ticksTo<Microseconds>(_endTime - _writingParticipantListStartTime);
    }
    return tickSource->ticksTo<Microseconds>(curTick - _writingParticipantListStartTime);
}

Microseconds SingleTransactionCoordinatorStats::getWritingParticipantListDuration(
    TickSource* tickSource, TickSource::Tick curTick) const {
    invariant(_writingParticipantListStartTime);

    if (_waitingForVotesStartTime) {
        return tickSource->ticksTo<Microseconds>(_waitingForVotesStartTime -
                                                 _writingParticipantListStartTime);
    }

    if (_endTime) {
        return tickSource->ticksTo<Microseconds>(_endTime - _writingParticipantListStartTime);
    }

    return tickSource->ticksTo<Microseconds>(curTick - _writingParticipantListStartTime);
}

Microseconds SingleTransactionCoordinatorStats::getWaitingForVotesDuration(
    TickSource* tickSource, TickSource::Tick curTick) const {
    invariant(_waitingForVotesStartTime);

    if (_writingDecisionStartTime) {
        return tickSource->ticksTo<Microseconds>(_writingDecisionStartTime -
                                                 _waitingForVotesStartTime);
    }

    if (_endTime) {
        return tickSource->ticksTo<Microseconds>(_endTime - _waitingForVotesStartTime);
    }

    return tickSource->ticksTo<Microseconds>(curTick - _waitingForVotesStartTime);
}

Microseconds SingleTransactionCoordinatorStats::getWritingDecisionDuration(
    TickSource* tickSource, TickSource::Tick curTick) const {
    invariant(_writingDecisionStartTime);

    if (_waitingForDecisionAcksStartTime) {
        return tickSource->ticksTo<Microseconds>(_waitingForDecisionAcksStartTime -
                                                 _writingDecisionStartTime);
    }

    if (_endTime) {
        return tickSource->ticksTo<Microseconds>(_endTime - _writingDecisionStartTime);
    }

    return tickSource->ticksTo<Microseconds>(curTick - _writingDecisionStartTime);
}

Microseconds SingleTransactionCoordinatorStats::getWaitingForDecisionAcksDuration(
    TickSource* tickSource, TickSource::Tick curTick) const {
    invariant(_waitingForDecisionAcksStartTime);

    if (_deletingCoordinatorDocStartTime) {
        return tickSource->ticksTo<Microseconds>(_deletingCoordinatorDocStartTime -
                                                 _waitingForDecisionAcksStartTime);
    }

    if (_endTime) {
        return tickSource->ticksTo<Microseconds>(_endTime - _waitingForDecisionAcksStartTime);
    }

    return tickSource->ticksTo<Microseconds>(curTick - _waitingForDecisionAcksStartTime);
}

Microseconds SingleTransactionCoordinatorStats::getDeletingCoordinatorDocDuration(
    TickSource* tickSource, TickSource::Tick curTick) const {
    invariant(_deletingCoordinatorDocStartTime);

    if (_endTime) {
        return tickSource->ticksTo<Microseconds>(_endTime - _deletingCoordinatorDocStartTime);
    }

    return tickSource->ticksTo<Microseconds>(curTick - _deletingCoordinatorDocStartTime);
}

void SingleTransactionCoordinatorStats::reportMetrics(BSONObjBuilder& parent,
                                                      TickSource* tickSource,
                                                      TickSource::Tick curTick) const {
    BSONObjBuilder stepDurationsBuilder;

    invariant(_createTime);
    parent.append("commitStartTime", _createWallClockTime);

    if (_writingParticipantListStartTime) {
        const auto statValue = getWritingParticipantListDuration(tickSource, curTick);
        stepDurationsBuilder.append("writingParticipantListMicros",
                                    durationCount<Microseconds>(statValue));

        const auto statValue2 = getTwoPhaseCommitDuration(tickSource, curTick);
        stepDurationsBuilder.append("totalCommitDurationMicros",
                                    durationCount<Microseconds>(statValue2));
    }

    if (_waitingForVotesStartTime) {
        const auto statValue = getWaitingForVotesDuration(tickSource, curTick);
        stepDurationsBuilder.append("waitingForVotesMicros",
                                    durationCount<Microseconds>(statValue));
    }

    if (_writingDecisionStartTime) {
        const auto statValue = getWritingDecisionDuration(tickSource, curTick);
        stepDurationsBuilder.append("writingDecisionMicros",
                                    durationCount<Microseconds>(statValue));
    }

    if (_waitingForDecisionAcksStartTime) {
        const auto statValue = getWaitingForDecisionAcksDuration(tickSource, curTick);
        stepDurationsBuilder.append("waitingForDecisionAcksMicros",
                                    durationCount<Microseconds>(statValue));
    }

    if (_deletingCoordinatorDocStartTime) {
        const auto statValue = getDeletingCoordinatorDocDuration(tickSource, curTick);
        stepDurationsBuilder.append("deletingCoordinatorDocMicros",
                                    durationCount<Microseconds>(statValue));
    }

    parent.append("stepDurations", stepDurationsBuilder.obj());
}

void SingleTransactionCoordinatorStats::reportLastClient(BSONObjBuilder& parent) const {
    parent.append("client", _lastClientInfo.clientHostAndPort);
    parent.append("host", getHostNameCachedAndPort());
    parent.append("connectionId", _lastClientInfo.connectionId);
    parent.append("appName", _lastClientInfo.appName);
    parent.append("clientMetadata", _lastClientInfo.clientMetadata);
}

}  // namespace mongo
