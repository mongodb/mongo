/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/vector_clock_mutable.h"

#include "mongo/logv2/log.h"

namespace mongo {
namespace {

const auto vectorClockMutableDecoration = ServiceContext::declareDecoration<VectorClockMutable*>();

}  // namespace

VectorClockMutable* VectorClockMutable::get(ServiceContext* service) {
    return vectorClockMutableDecoration(service);
}

VectorClockMutable* VectorClockMutable::get(OperationContext* ctx) {
    return get(ctx->getClient()->getServiceContext());
}

VectorClockMutable::VectorClockMutable() = default;

VectorClockMutable::~VectorClockMutable() = default;

void VectorClockMutable::registerVectorClockOnServiceContext(
    ServiceContext* service, VectorClockMutable* vectorClockMutable) {
    VectorClock::registerVectorClockOnServiceContext(service, vectorClockMutable);
    auto& clock = vectorClockMutableDecoration(service);
    invariant(!clock);
    clock = std::move(vectorClockMutable);
}

LogicalTime VectorClockMutable::_advanceComponentTimeByTicks(Component component, uint64_t nTicks) {
    invariant(nTicks > 0 && nTicks <= kMaxValue);

    stdx::lock_guard<Latch> lock(_mutex);

    LogicalTime time = _vectorTime[component];

    const unsigned wallClockSecs =
        durationCount<Seconds>(_service->getFastClockSource()->now().toDurationSinceEpoch());
    unsigned timeSecs = time.asTimestamp().getSecs();

    // Synchronize time with wall clock time, if time was behind in seconds.
    if (timeSecs < wallClockSecs) {
        time = LogicalTime(Timestamp(wallClockSecs, 0));
    }
    // If reserving 'nTicks' would force the time's increment field to exceed (2^31-1),
    // overflow by moving to the next second. We use the signed integer maximum as an overflow point
    // in order to preserve compatibility with potentially signed or unsigned integral Timestamp
    // increment types. It is also unlikely to tick a clock by more than 2^31 in the span of one
    // second.
    else if (time.asTimestamp().getInc() > (kMaxValue - nTicks)) {

        LOGV2(20709,
              "Exceeded maximum allowable increment value within one second. Moving time forward "
              "to the next second.",
              "vectorClockComponent"_attr = _componentName(component));

        // Move time forward to the next second
        time = LogicalTime(Timestamp(time.asTimestamp().getSecs() + 1, 0));
    }

    uassert(40482,
            str::stream() << _componentName(component)
                          << " cannot be advanced beyond the maximum logical time value",
            _lessThanOrEqualToMaxPossibleTime(time, nTicks));

    // Save the next time.
    time.addTicks(1);
    _vectorTime[component] = time;

    // Add the rest of the requested ticks if needed.
    if (nTicks > 1) {
        _vectorTime[component].addTicks(nTicks - 1);
    }

    return time;
}

void VectorClockMutable::_advanceComponentTimeTo(Component component, LogicalTime&& newTime) {
    stdx::lock_guard<Latch> lock(_mutex);

    // Rate limit checks are skipped here so a server with no activity for longer than
    // maxAcceptableLogicalClockDriftSecs seconds can still have its cluster time initialized.
    uassert(40483,
            str::stream() << _componentName(component)
                          << " cannot be advanced beyond the maximum logical time value",
            _lessThanOrEqualToMaxPossibleTime(newTime, 0));

    if (newTime > _vectorTime[component]) {
        _vectorTime[component] = std::move(newTime);
    }
}

}  // namespace mongo
