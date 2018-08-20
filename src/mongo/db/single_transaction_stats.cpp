/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/single_transaction_stats.h"

namespace mongo {

unsigned long long SingleTransactionStats::getStartTime() const {
    invariant(_startTime > 0);

    return _startTime;
}

void SingleTransactionStats::setStartTime(unsigned long long time) {
    invariant(_startTime == 0);

    _startTime = time;
}

unsigned long long SingleTransactionStats::getDuration(unsigned long long curTime) const {
    invariant(_startTime > 0);

    // The transaction hasn't ended yet, so we return how long it has currently been running for.
    if (_endTime == 0) {
        return curTime - _startTime;
    }
    return _endTime - _startTime;
}

void SingleTransactionStats::setEndTime(unsigned long long time) {
    invariant(_startTime > 0);

    _endTime = time;
}

Microseconds SingleTransactionStats::getTimeActiveMicros(unsigned long long curTime) const {
    invariant(_startTime > 0);

    // The transaction is currently active, so we return the recorded active time so far plus the
    // time since _timeActiveStart.
    if (isActive()) {
        return _timeActiveMicros +
            Microseconds{static_cast<long long>(curTime - _lastTimeActiveStart)};
    }
    return _timeActiveMicros;
}

Microseconds SingleTransactionStats::getTimeInactiveMicros(unsigned long long curTime) const {
    invariant(_startTime > 0);

    return Microseconds{static_cast<long long>(getDuration(curTime))} -
        getTimeActiveMicros(curTime);
}

void SingleTransactionStats::setActive(unsigned long long time) {
    invariant(!isActive());

    _lastTimeActiveStart = time;
}

void SingleTransactionStats::setInactive(unsigned long long time) {
    invariant(isActive());

    _timeActiveMicros += Microseconds{static_cast<long long>(time - _lastTimeActiveStart)};
    _lastTimeActiveStart = 0;
}

void SingleTransactionStats::report(BSONObjBuilder* builder,
                                    const repl::ReadConcernArgs& readConcernArgs) const {
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
    builder->append("startWallClockTime",
                    dateToISOStringLocal(Date_t::fromMillisSinceEpoch(getStartTime() / 1000)));

    // We use the same "now" time so that the following time metrics are consistent with each other.
    auto curTime = curTimeMicros64();
    builder->append("timeOpenMicros", static_cast<long long>(getDuration(curTime)));

    auto timeActive = durationCount<Microseconds>(getTimeActiveMicros(curTime));
    auto timeInactive = durationCount<Microseconds>(getTimeInactiveMicros(curTime));

    builder->append("timeActiveMicros", timeActive);
    builder->append("timeInactiveMicros", timeInactive);

    if (_expireDate != Date_t::max()) {
        builder->append("expiryTime", dateToISOStringLocal(_expireDate));
    }
}

}  // namespace mongo
