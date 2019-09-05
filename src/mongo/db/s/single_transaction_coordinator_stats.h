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

#include "mongo/db/client.h"
#include "mongo/rpc/metadata/client_metadata_ismaster.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * Tracks metrics for a single transaction coordinator.
 *
 * For all timing related stats, a TickSource with at least microsecond resolution must be used.
 */
class SingleTransactionCoordinatorStats {
public:
    SingleTransactionCoordinatorStats() = default;

    struct LastClientInfo {
        std::string clientHostAndPort;
        long long connectionId;
        BSONObj clientMetadata;
        std::string appName;

        void update(Client* client) {
            if (client->hasRemote()) {
                clientHostAndPort = client->getRemote().toString();
            }
            connectionId = client->getConnectionId();
            if (const auto& metadata =
                    ClientMetadataIsMasterState::get(client).getClientMetadata()) {
                clientMetadata = metadata.get().getDocument();
                appName = metadata.get().getApplicationName().toString();
            }
        }
    };

    //
    // Setters
    //

    /**
     * Sets the time the transaction coordinator was created.
     *
     * Can only be called once.
     */
    void setCreateTime(TickSource::Tick curTick, Date_t curWallClockTime);

    /**
     * Sets the time the transaction coordinator was destroyed.
     *
     * Can only be called once, and must be called after setCreateTime.
     */
    void setEndTime(TickSource::Tick curTick, Date_t curWallClockTime);

    /**
     * Sets the time the transaction coordinator wrote the participant list and started waiting for
     * the participant list to become majority-committed.
     *
     * Can only be called once, and must be called after setCreateTime.
     */
    void setWritingParticipantListStartTime(TickSource::Tick curTick, Date_t curWallClockTime);

    /**
     * Sets the time the transaction coordinator sent 'prepare' to the participants and started
     * waiting for votes (i.e., 'prepare' responses).
     *
     * Can only be called once, and must be called after setWritingParticipantListStartTime.
     */
    void setWaitingForVotesStartTime(TickSource::Tick curTick, Date_t curWallClockTime);

    /**
     * Sets the time the transaction coordinator wrote the decision and started waiting for the
     * decision to become majority-committed.
     *
     * Can only be called once, and must be called after setWaitingForVotesStartTime.
     */
    void setWritingDecisionStartTime(TickSource::Tick curTick, Date_t curWallClockTime);

    /**
     * Sets the time the transaction coordinator sent the decision to the participants and started
     * waiting for acknowledgments.
     *
     * Can only be called once, and must be called after setWritingDecisionStartTime.
     */
    void setWaitingForDecisionAcksStartTime(TickSource::Tick curTick, Date_t curWallClockTime);

    /**
     * Sets the time the transaction coordinator deleted its durable state.
     *
     * Can only be called once, and must be called after setWaitingForDecisionAcksStartTime.
     */
    void setDeletingCoordinatorDocStartTime(TickSource::Tick curTick, Date_t curWallClockTime);

    //
    // Getters
    //

    /**
     * Returns the time the coordinator was created.
     *
     * Must be called after setCreateTime.
     */
    Date_t getCreateTime() const {
        return _createWallClockTime;
    }

    /**
     * Returns the time the coordinator was destroyed.
     *
     * Must be called after setCreateTime.
     */
    Date_t getEndTime() const {
        return _endWallClockTime;
    }

    /**
     * Returns the time the coordinator started writing the participant list. Note, this is also the
     * two-phase commit start time.
     *
     * Must be called after setWritingParticipantListStartTime.
     */
    Date_t getWritingParticipantListStartTime() const {
        return _writingParticipantListStartWallClockTime;
    }

    /**
     * Returns the time the coordinator started sending 'prepare' and collecting votes.
     *
     * Must be called after setWaitingForVotesStartTime.
     */
    Date_t getWaitingForVotesStartTime() const {
        return _waitingForVotesStartWallClockTime;
    }

    /**
     * Returns the time the coordinator started making the decision durable.
     *
     * Must be called after setWritingDecisionStartTime.
     */
    Date_t getWritingDecisionStartTime() const {
        return _writingDecisionStartWallClockTime;
    }

    /**
     * Returns the time the coordinator started sending the decision and waiting for
     * acknowledgments.
     *
     * Must be called after setWaitingForDecisionAcksStartTime.
     */
    Date_t getWaitingForDecisionAcksStartTime() const {
        return _waitingForDecisionAcksStartWallClockTime;
    }

    /**
     * Returns the time the coordinator started deleting its durable state.
     *
     * Must be called after setDeletingCoordinatorDocStartTime.
     */
    Date_t getDeletingCoordinatorDocStartTime() const {
        return _deletingCoordinatorDocStartWallClockTime;
    }

    /**
     * If the end time has been set, returns the duration between the create time and end time, else
     * returns the duration between the create time and curTick.
     *
     * Must be called after setCreateTime, but can be called any number of times.
     */
    Microseconds getDurationSinceCreation(TickSource* tickSource, TickSource::Tick curTick) const;

    /**
     * If the end time has been set, returns the duration between the writing participant list start
     * time and end time, else returns the duration between the writing participant list start time
     * and curTick.
     *
     * Must be called after setWritingParticipantListStartTime, but can be called any number of
     * times.
     */
    Microseconds getTwoPhaseCommitDuration(TickSource* tickSource, TickSource::Tick curTick) const;

    /**
     * If the waiting for votes start time has been set, returns the duration between the writing
     * participant list start time and the waiting for votes start time, else returns the duration
     * between the writing for participant list start time and curTick.
     *
     * Must be called after setWritingParticipantListStartTime, but can be called any number of
     * times.
     */
    Microseconds getWritingParticipantListDuration(TickSource* tickSource,
                                                   TickSource::Tick curTick) const;

    /**
     * If the writing decision start time has been set, returns the duration between the waiting for
     * votes start time and the writing decision start time, else returns the duration between
     * the waiting for votes start time and curTick.
     *
     * Must be called after setWaitingForVotesStartTime, but can be called any number of times.
     */
    Microseconds getWaitingForVotesDuration(TickSource* tickSource, TickSource::Tick curTick) const;

    /**
     * If the waiting for decision acks start time has been set, returns the duration between the
     * writing decision start time and the waiting for decision acks start time, else returns the
     * duration between the writing decision start time and curTick.
     *
     * Must be called after setWritingDecisionStartTime, but can be called any number of times.
     */
    Microseconds getWritingDecisionDuration(TickSource* tickSource, TickSource::Tick curTick) const;

    /**
     * If the deleting coordinator doc start time has been set, returns the duration between the
     * waiting for decision acks start time and the deleting coordinator doc start time, else
     * returns the duration between the waiting for decision acks start time and curTick.
     *
     * Must be called after setWaitingForDecisionAcksStartTime, but can be called any number of
     * times.
     */
    Microseconds getWaitingForDecisionAcksDuration(TickSource* tickSource,
                                                   TickSource::Tick curTick) const;

    /**
     * If the end time has been set, returns the duration between the deleting coordinator doc start
     * and the end time, else returns the duration between the deleting coordinator doc start time
     * and curTick.
     *
     * Must be called after setDeletingCoordinatorDocStartTime, but can be called any number of
     * times.
     */
    Microseconds getDeletingCoordinatorDocDuration(TickSource* tickSource,
                                                   TickSource::Tick curTick) const;

    /**
     * Reports the time duration for each step in the two-phase commit and stores them as a
     * sub-document of the provided parent BSONObjBuilder. The metrics are stored under key
     * "stepDurations" in the parent document.
     */
    void reportMetrics(BSONObjBuilder& parent,
                       TickSource* tickSource,
                       TickSource::Tick curTick) const;

    /**
     * Reports information about the last client to interact with this transaction.
     */
    void reportLastClient(BSONObjBuilder& parent) const;

    /**
     * Updates the LastClientInfo object stored in this SingleTransactionStats instance with the
     * given Client's information.
     */
    void updateLastClientInfo(Client* client) {
        invariant(client);
        _lastClientInfo.update(client);
    }

private:
    Date_t _createWallClockTime;
    TickSource::Tick _createTime{0};

    // The writing participant list start time doubles as the two-phase commit start time, since
    // writing the participant list is the first step of the two-phase commit.
    Date_t _writingParticipantListStartWallClockTime;
    TickSource::Tick _writingParticipantListStartTime{0};

    Date_t _waitingForVotesStartWallClockTime;
    TickSource::Tick _waitingForVotesStartTime{0};

    Date_t _writingDecisionStartWallClockTime;
    TickSource::Tick _writingDecisionStartTime{0};

    Date_t _waitingForDecisionAcksStartWallClockTime;
    TickSource::Tick _waitingForDecisionAcksStartTime{0};

    Date_t _deletingCoordinatorDocStartWallClockTime;
    TickSource::Tick _deletingCoordinatorDocStartTime{0};

    Date_t _endWallClockTime;
    TickSource::Tick _endTime{0};

    LastClientInfo _lastClientInfo;
};

}  // namespace mongo
