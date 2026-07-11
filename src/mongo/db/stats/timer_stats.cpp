// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
