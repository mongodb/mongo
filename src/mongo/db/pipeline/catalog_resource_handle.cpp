// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/catalog_resource_handle.h"

#include "mongo/db/curop_metrics.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(sleepAfterReleasingAggTicket);

void DSCatalogResourceHandleBase::acquire(OperationContext* opCtx) {
    tassert(12779600, "Expected resources to be absent", !_resources);
    closeAggNonTicketedIntervalIfOpen(getAggNonTicketedIntervalTracker(opCtx), opCtx);
    _lastOpCtx = opCtx;
    _resources.emplace(opCtx, _transactionResourcesStasher.get());
}

void DSCatalogResourceHandleBase::release() {
    _resources.reset();

    // Record the start of a non-ticketed interval. The pipeline is about to do in-memory work
    // (e.g. $sort, $group) without holding an execution ticket. The interval will be closed by
    // recordCurOpMetrics() at command end.
    if (_lastOpCtx) {
        auto& tracker = getAggNonTicketedIntervalTracker(_lastOpCtx);
        tracker.openInterval(_lastOpCtx->tickSource().getTicks());
    }

    sleepAfterReleasingAggTicket.executeIf(
        [](const BSONObj& data) { sleepFor(Milliseconds(data["waitTimeMillis"].safeNumberInt())); },
        [](const BSONObj&) { return true; });
}

}  // namespace mongo
