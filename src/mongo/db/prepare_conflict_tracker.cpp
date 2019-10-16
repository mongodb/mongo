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

#include "mongo/db/prepare_conflict_tracker.h"
#include "mongo/platform/basic.h"

namespace mongo {

const OperationContext::Decoration<PrepareConflictTracker> PrepareConflictTracker::get =
    OperationContext::declareDecoration<PrepareConflictTracker>();

bool PrepareConflictTracker::isWaitingOnPrepareConflict() const {
    return _waitOnPrepareConflict.load();
}

void PrepareConflictTracker::beginPrepareConflict(OperationContext* opCtx) {
    invariant(_prepareConflictStartTime == 0);
    _prepareConflictStartTime = opCtx->getServiceContext()->getTickSource()->getTicks();

    // Implies that the current read operation is blocked on a prepared transaction.
    _waitOnPrepareConflict.store(true);
}

void PrepareConflictTracker::endPrepareConflict(OperationContext* opCtx) {
    // This function is called regardless whether there was a prepare conflict.
    if (_prepareConflictStartTime) {
        auto tickSource = opCtx->getServiceContext()->getTickSource();
        auto curTick = tickSource->getTicks();

        invariant(_prepareConflictStartTime <= curTick,
                  str::stream() << "Prepare conflict start time ("
                                << tickSource->ticksTo<Microseconds>(_prepareConflictStartTime)
                                << ") is somehow greater than current time ("
                                << tickSource->ticksTo<Microseconds>(curTick) << ")");

        auto curConflictDuration =
            tickSource->ticksTo<Microseconds>(curTick - _prepareConflictStartTime);
        _prepareConflictDuration += curConflictDuration;
    }
    _prepareConflictStartTime = 0;

    // Implies that the current read operation is not blocked on a prepared transaction.
    _waitOnPrepareConflict.store(false);
}

Microseconds PrepareConflictTracker::getPrepareConflictDuration() {
    return _prepareConflictDuration;
}

}  // namespace mongo
