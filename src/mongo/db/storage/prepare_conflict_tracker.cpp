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

#include "mongo/db/storage/prepare_conflict_tracker.h"

#include "mongo/db/commands/server_status_metric.h"

#include <utility>

namespace mongo {

namespace {

auto& prepareConflictWaitMicros = *MetricBuilder<Counter64>("operation.prepareConflictWaitMicros");
auto& prepareConflicts = *MetricBuilder<Counter64>{"operation.prepareConflicts"};
}  // namespace

bool PrepareConflictTracker::isWaitingOnPrepareConflict() const {
    return _waitingOnPrepareConflict.load();
}

void PrepareConflictTracker::beginPrepareConflict(TickSource& tickSource) {
    invariant(!_waitingOnPrepareConflict.load());
    _waitingOnPrepareConflict.store(true);
    prepareConflicts.increment();
    _numPrepareConflictsThisOp.fetchAndAddRelaxed(1);
    _prepareConflictLastUpdateTime = tickSource.getTicks();
}

void PrepareConflictTracker::updatePrepareConflict(TickSource& tickSource) {
    invariant(_waitingOnPrepareConflict.load());

    auto curTick = tickSource.getTicks();

    auto increment = tickSource.ticksTo<Microseconds>(curTick - _prepareConflictLastUpdateTime);
    _thisOpPrepareConflictDuration.fetchAndAddRelaxed(durationCount<Microseconds>(increment));

    prepareConflictWaitMicros.increment(increment.count());
    _prepareConflictLastUpdateTime = curTick;
}

void PrepareConflictTracker::endPrepareConflict(TickSource& tickSource) {
    updatePrepareConflict(tickSource);
    _waitingOnPrepareConflict.store(false);
}

long long PrepareConflictTracker::getThisOpPrepareConflictCount() const {
    return _numPrepareConflictsThisOp.loadRelaxed();
}

Microseconds PrepareConflictTracker::getThisOpPrepareConflictDuration() {
    return Microseconds{_thisOpPrepareConflictDuration.loadRelaxed()};
}

long long PrepareConflictTracker::getGlobalNumPrepareConflicts() {
    return prepareConflicts.get();
}

long long PrepareConflictTracker::getGlobalWaitingForPrepareConflictsMicros() {
    return prepareConflictWaitMicros.get();
}

void PrepareConflictTracker::resetGlobalPrepareConflictStats() {
    prepareConflicts.setToZero();
    prepareConflictWaitMicros.setToZero();
}
}  // namespace mongo
