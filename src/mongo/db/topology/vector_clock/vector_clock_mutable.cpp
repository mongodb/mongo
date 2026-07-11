// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <mutex>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

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

    std::lock_guard lock(_mutex);

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

    // storeRelaxed is sufficient: _mutex's unlock provides release semantics for later
    // acquirers, and the _advanceTime precheck (vector_clock.cpp) tolerates staleness by design.
    _vectorTimeShadow[component].storeRelaxed(_vectorTime[component].asTimestamp().asULL());

    return time;
}

void VectorClockMutable::_advanceComponentTimeTo(Component component, LogicalTime&& newTime) {
    std::lock_guard lock(_mutex);

    // Rate limit checks are skipped here so a server with no activity for longer than
    // maxAcceptableLogicalClockDriftSecs seconds can still have its cluster time initialized.
    uassert(40483,
            str::stream() << _componentName(component)
                          << " cannot be advanced beyond the maximum logical time value",
            _lessThanOrEqualToMaxPossibleTime(newTime, 0));

    if (newTime > _vectorTime[component]) {
        _vectorTime[component] = std::move(newTime);
        _vectorTimeShadow[component].storeRelaxed(_vectorTime[component].asTimestamp().asULL());
    }
}

}  // namespace mongo
