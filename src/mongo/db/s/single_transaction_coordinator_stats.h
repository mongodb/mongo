// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/s/transaction_coordinator.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
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
