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

#include "mongo/platform/basic.h"

#include "mongo/db/stats/single_transaction_stats.h"

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
        invariant(_preparedStartTime.get() > 0);
        if (_endTime == 0) {
            return tickSource->ticksTo<Microseconds>(curTick - _preparedStartTime.get());
        }
        return tickSource->ticksTo<Microseconds>(_endTime - _preparedStartTime.get());
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
    parametersBuilder.append("txnNumber", _txnNumber);

    if (!isForMultiDocumentTransaction()) {
        // For retryable writes, we only include the txnNumber.
        parametersBuilder.done();
        return;
    }

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
}

}  // namespace mongo
