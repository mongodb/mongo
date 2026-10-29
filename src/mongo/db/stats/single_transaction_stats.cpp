// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/stats/single_transaction_stats.h"

#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

void SingleTransactionStats::setStartTime(TickSource::Tick curTick, Date_t curWallClockTime) {
    invariant(_startTime == 0);

    _startTime = curTick;
    _startWallClockTime = curWallClockTime;
}

Microseconds SingleTransactionStats::getDuration(TickSource* tickSource,
                                                 TickSource::Tick curTick) const {
    invariant(_startTime > 0);

    // The transaction hasn't ended yet, so we return how long it has currently been running for.
    if (_endTime == 0) {
        return tickSource->ticksTo<Microseconds>(curTick - _startTime);
    }
    return tickSource->ticksTo<Microseconds>(_endTime - _startTime);
}

Microseconds SingleTransactionStats::getPreparedDuration(TickSource* tickSource,
                                                         TickSource::Tick curTick) const {
    invariant(_startTime > 0);
    if (_preparedStartTime != boost::none) {
        // If the transaction hasn't ended yet, we return how long it has currently been running
        // for.
        invariant(_preparedStartTime.value() > 0);
        if (_endTime == 0) {
            return tickSource->ticksTo<Microseconds>(curTick - _preparedStartTime.value());
        }
        return tickSource->ticksTo<Microseconds>(_endTime - _preparedStartTime.value());
    }
    return Microseconds(0);
}

void SingleTransactionStats::setPreparedStartTime(TickSource::Tick time) {
    invariant(_startTime > 0);

    _preparedStartTime = time;
}

void SingleTransactionStats::setEndTime(TickSource::Tick time) {
    invariant(_startTime > 0);

    _endTime = time;
}

Microseconds SingleTransactionStats::getTimeActiveMicros(TickSource* tickSource,
                                                         TickSource::Tick curTick) const {
    invariant(_startTime > 0);

    // The transaction is currently active, so we return the recorded active time so far plus the
    // time since _timeActiveStart.
    if (isActive()) {
        return _timeActiveMicros +
            tickSource->ticksTo<Microseconds>(curTick - _lastTimeActiveStart);
    }
    return _timeActiveMicros;
}

Microseconds SingleTransactionStats::getTimeInactiveMicros(TickSource* tickSource,
                                                           TickSource::Tick curTick) const {
    invariant(_startTime > 0);

    return getDuration(tickSource, curTick) - getTimeActiveMicros(tickSource, curTick);
}

void SingleTransactionStats::setActive(TickSource::Tick curTick) {
    invariant(!isActive());

    _lastTimeActiveStart = curTick;
}

void SingleTransactionStats::setInactive(TickSource* tickSource, TickSource::Tick curTick) {
    invariant(isActive());

    _timeActiveMicros += tickSource->ticksTo<Microseconds>(curTick - _lastTimeActiveStart);
    _lastTimeActiveStart = 0;
}

void SingleTransactionStats::report(BSONObjBuilder* builder,
                                    const repl::ReadConcernArgs& readConcernArgs,
                                    TickSource* tickSource,
                                    TickSource::Tick curTick) const {
    BSONObjBuilder parametersBuilder(builder->subobjStart("parameters"));
    parametersBuilder.append("txnNumber", _txnNumberAndRetryCounter.getTxnNumber());

    if (!isForMultiDocumentTransaction()) {
        // For retryable writes, we only include the txnNumber.
        parametersBuilder.done();
        return;
    }

    parametersBuilder.append("txnRetryCounter", *_txnNumberAndRetryCounter.getTxnRetryCounter());
    parametersBuilder.append("autocommit", *_autoCommit);
    readConcernArgs.appendInfo(&parametersBuilder);
    parametersBuilder.done();

    builder->append("readTimestamp", _readTimestamp);
    builder->append("startWallClockTime", dateToISOStringLocal(_startWallClockTime));

    // The same "now" time must be used so that the following time metrics are consistent with each
    // other.
    builder->append("timeOpenMicros",
                    durationCount<Microseconds>(getDuration(tickSource, curTick)));

    auto timeActive = durationCount<Microseconds>(getTimeActiveMicros(tickSource, curTick));
    auto timeInactive = durationCount<Microseconds>(getTimeInactiveMicros(tickSource, curTick));

    builder->append("timeActiveMicros", timeActive);
    builder->append("timeInactiveMicros", timeInactive);

    if (_preparedStartTime != boost::none) {
        auto timePrepared = durationCount<Microseconds>(getPreparedDuration(tickSource, curTick));
        builder->append("timePreparedMicros", timePrepared);
    }

    if (_expireDate != Date_t::max()) {
        builder->append("expiryTime", dateToISOStringLocal(_expireDate));
    }

    if (_recoveredFromPreciseCheckpoint) {
        builder->append("recoveredFromPreciseCheckpoint", true);
    }
}

}  // namespace mongo
