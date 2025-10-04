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

#include "mongo/db/stats/timer_stats.h"

#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

TimerHolder::TimerHolder(TimerStats* stats) : _stats(stats), _recorded(false) {}

TimerHolder::~TimerHolder() {
    if (!_recorded) {
        recordMillis();
    }
}

int TimerHolder::recordMillis() {
    _recorded = true;
    if (_stats) {
        return _stats->record(_t);
    }
    return _t.millis();
}

void TimerStats::recordMillis(int millis) {
    _num.fetchAndAdd(1);
    _totalMillis.fetchAndAdd(millis);
}

int TimerStats::record(const Timer& timer) {
    int millis = timer.millis();
    recordMillis(millis);
    return millis;
}

BSONObj TimerStats::getReport() const {
    long long n, t;
    {
        n = _num.loadRelaxed();
        t = _totalMillis.loadRelaxed();
    }
    BSONObjBuilder b(64);
    b.appendNumber("num", n);
    b.appendNumber("totalMillis", t);
    return b.obj();
}
}  // namespace mongo
