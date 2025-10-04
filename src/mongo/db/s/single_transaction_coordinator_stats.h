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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/s/transaction_coordinator.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"

#include <string>

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
            if (auto metadata = ClientMetadata::get(client)) {
                clientMetadata = metadata->getDocument();
                appName = std::string{metadata->getApplicationName()};
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
     * Sets the time the transaction coordinator started given step.
     *
     * Can only be called once per each step and must be called in order.
     */
    void setStepStartTime(TransactionCoordinator::Step step,
                          TickSource::Tick curTick,
                          Date_t curWallClockTime);

    //
    // Getters
    //

    /**
     * Returns the time the coordinator was created.
     *
     * Must be called after setCreateTime.
     */
    Date_t getCreateTime() const {
        return _wallClockTimes[createIndex()];
    }

    /**
     * Returns the time the coordinator was destroyed.
     *
     * Must be called after setCreateTime.
     */
    Date_t getEndTime() const {
        return _wallClockTimes[endIndex()];
    }

    /**
     * Returns the time the coordinator started the given step.
     *
     * Must be called after setStepStartTime with the same step.
     */
    Date_t getStepStartTime(TransactionCoordinator::Step step) const {
        return _wallClockTimes[static_cast<size_t>(step)];
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
     * Must be called after setStepStartTime for kWritingParticipantList step, but can be called any
     * number of times.
     */
    Microseconds getTwoPhaseCommitDuration(TickSource* tickSource, TickSource::Tick curTick) const;

    /**
     * If the start time for the next step is set, return duration between start time of the given
     * step and the next step. Otherwise, return the duration between the start of the given step
     * and curTime.
     *
     * Must be called after setStepStartTime for the given step, but can be called any number of
     * times.
     */
    Microseconds getStepDuration(TransactionCoordinator::Step step,
                                 TickSource* tickStoure,
                                 TickSource::Tick curTime) const;

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
    void reportLastClient(OperationContext* opCtx, BSONObjBuilder& parent) const;

    /**
     * Updates the LastClientInfo object stored in this SingleTransactionStats instance with the
     * given Client's information.
     */
    void updateLastClientInfo(Client* client) {
        invariant(client);
        _lastClientInfo.update(client);
    }
    /*
     * Marks this transaction coordinator has having recovered from failure.
     */
    void setRecoveredFromFailover();

private:
    bool hasTime(TransactionCoordinator::Step step) const {
        return _times[static_cast<size_t>(step)];
    }

    static size_t endIndex() {
        return static_cast<size_t>(TransactionCoordinator::Step::kLastStep) + 1;
    }

    static size_t createIndex() {
        return 0;
    }

    std::vector<Date_t> _wallClockTimes = std::vector<Date_t>(endIndex() + 1);
    std::vector<TickSource::Tick> _times = std::vector<TickSource::Tick>(endIndex() + 1, 0);

    LastClientInfo _lastClientInfo;
    bool _hasRecoveredFromFailover = false;
};

}  // namespace mongo
