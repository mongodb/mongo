/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
