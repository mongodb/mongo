// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/prepare_conflict_tracker.h"

#include "mongo/db/commands/server_status/server_status_metric.h"

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
