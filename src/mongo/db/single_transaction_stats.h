
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/curop.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/metadata/client_metadata_ismaster.h"

namespace mongo {

/**
 * Tracks metrics for a single multi-document transaction.
 *
 * For all timing related stats, a TickSource with at least microsecond resolution must be used.
 */
class SingleTransactionStats {
public:
    /**
     * Stores information about the last client to run a transaction operation.
     */
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

    SingleTransactionStats() : _txnNumber(kUninitializedTxnNumber){};
    SingleTransactionStats(TxnNumber txnNumber) : _txnNumber(txnNumber){};

    /**
     * Sets the transaction's start time, only if it hasn't already been set.
     *
     * This method takes two sources of input for the current time. The 'curTick' argument should be
     * the current time as measured by a TickSource, which is a high precision interface for
     * measuring the passage of time that should provide at least microsecond resolution. The given
     * 'curWallClockTime' need only be a millisecond resolution time point that serves as a close
     * approximation to the true start time. This millisecond resolution time point is not used for
     * measuring transaction durations. The given tick value, however, is.
     *
     * This method must only be called once.
     */
    void setStartTime(TickSource::Tick curTick, Date_t curWallClockTime);

    /**
     * If the transaction is currently in progress, this method returns the duration
     * the transaction has been running for in microseconds, given the current time value.
     *
     * For a completed transaction, this method returns the total duration of the
     * transaction in microseconds.
     *
     * This method cannot be called until setStartTime() has been called.
     */
    Microseconds getDuration(TickSource* tickSource, TickSource::Tick curTick) const;

    /**
     * If the transaction is currently in progress, this method returns the duration
     * the transaction has been in the prepared state for in microseconds, given the
     * current time value.
     *
     * For a completed transaction, this method returns the total duration the transaction
     * has been in the prepared state in microseconds.
     *
     * This method cannot be called until setStartTime() and setPreparedStartTime() have been
     * called.
     */
    Microseconds getPreparedDuration(TickSource* tickSource, TickSource::Tick curTick) const;

    /**
     * Sets the time at which a transaction enters the prepared state.
     *
     * This method cannot be called until setStartTime() has been called.
     */
    void setPreparedStartTime(TickSource::Tick time);

    /**
     * Sets the transaction's end time, only if the start time has already been set.
     *
     * This method cannot be called until setStartTime() has been called.
     */
    void setEndTime(TickSource::Tick time);

    /**
     * Returns the total active time of the transaction, given the current time value. A transaction
     * is active when there is a running operation that is part of the transaction.
     */
    Microseconds getTimeActiveMicros(TickSource* tickSource, TickSource::Tick curTick) const;

    /**
     * Returns the total inactive time of the transaction, given the current time value. A
     * transaction is inactive when it is idly waiting for a new operation to occur.
     */
    Microseconds getTimeInactiveMicros(TickSource* tickSource, TickSource::Tick curTick) const;

    /**
     * Marks the transaction as active and sets the start of the transaction's active time.
     *
     * This method cannot be called if the transaction is currently active. A call to setActive()
     * must be followed by a call to setInactive() before calling setActive() again.
     */
    void setActive(TickSource::Tick curTick);

    /**
     * Marks the transaction as inactive and sets the total active time of the transaction. The
     * total active time will only be set if the transaction was active prior to this call.
     *
     * This method cannot be called if the transaction is currently not active.
     */
    void setInactive(TickSource* tickSource, TickSource::Tick curTick);

    /**
     * Returns whether or not the transaction is currently active.
     */
    bool isActive() const {
        return _lastTimeActiveStart != 0;
    }

    /**
     * Returns whether or not the transaction has ended (aborted or committed).
     */
    bool isEnded() const {
        return _endTime != 0;
    }

    /**
     * Returns whether these stats are for a multi-document transaction.
     */
    bool isForMultiDocumentTransaction() const {
        return _autoCommit != boost::none;
    }

    /**
     * Returns the OpDebug object stored in this SingleTransactionStats instance.
     */
    OpDebug* getOpDebug() {
        return &_opDebug;
    }

    /**
     * Returns the LastClientInfo object stored in this SingleTransactionStats instance.
     */
    const LastClientInfo& getLastClientInfo() const {
        return _lastClientInfo;
    }

    /**
     * Updates the LastClientInfo object stored in this SingleTransactionStats instance with the
     * given Client's information.
     */
    void updateLastClientInfo(Client* client) {
        _lastClientInfo.update(client);
    }

    /**
     * Set the autoCommit field.  If this field is unset, this is not a transaction but a
     * retryable write and other values will not be meaningful.
     */
    void setAutoCommit(boost::optional<bool> autoCommit) {
        _autoCommit = autoCommit;
    }

    /**
     * Set the transaction expiration date.
     */
    void setExpireDate(Date_t expireDate) {
        _expireDate = expireDate;
    }

    /**
     * Set the transaction storage read timestamp.
     */
    void setReadTimestamp(Timestamp readTimestamp) {
        _readTimestamp = readTimestamp;
    }

    /**
     * Append the stats to the builder.
     */
    void report(BSONObjBuilder* builder,
                const repl::ReadConcernArgs& readConcernArgs,
                TickSource* tickSource,
                TickSource::Tick curTick) const;

private:
    // The transaction number of the transaction.
    TxnNumber _txnNumber;

    // Unset for retryable write, 'false' for multi-document transaction.  Value 'true' is
    // for future use.
    boost::optional<bool> _autoCommit;

    // The start time of the transaction in millisecond resolution. Used only for diagnostics
    // reporting. Not used for measuring transaction durations.
    Date_t _startWallClockTime;

    // The start time of the transaction. Note that tick values should only ever be used to measure
    // distance from other tick values, not for reporting absolute wall clock time.
    TickSource::Tick _startTime{0};

    // The end time of the transaction.
    TickSource::Tick _endTime{0};

    // The total amount of active time spent by the transaction.
    Microseconds _timeActiveMicros = Microseconds{0};

    // The time at which the transaction was last marked as active. The transaction is considered
    // active if this value is not equal to 0.
    TickSource::Tick _lastTimeActiveStart{0};

    // The expiration date of the transaction.
    Date_t _expireDate = Date_t::max();

    // The storage read timestamp of the transaction.
    Timestamp _readTimestamp;

    // Tracks and accumulates stats from all operations that run inside the transaction.
    OpDebug _opDebug;

    // Holds information about the last client to run a transaction operation.
    LastClientInfo _lastClientInfo;

    // The time at which a transaction becomes prepared. It is possible for _preparedStartTime to
    // not be set in a transaction that is in state kPrepared if an exception is thrown after the
    // transaction transitions to the prepared state but before setPreparedStartTime is called.
    boost::optional<TickSource::Tick> _preparedStartTime{boost::none};
};

}  // namespace mongo
