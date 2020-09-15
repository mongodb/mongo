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

#include "mongo/db/stats/resource_consumption_metrics.h"

#include "mongo/db/stats/operation_resource_consumption_gen.h"

namespace mongo {
namespace {
const OperationContext::Decoration<ResourceConsumption::MetricsCollector> getMetricsCollector =
    OperationContext::declareDecoration<ResourceConsumption::MetricsCollector>();
const ServiceContext::Decoration<ResourceConsumption> getGlobalResourceConsumption =
    ServiceContext::declareDecoration<ResourceConsumption>();
}  // namespace


ResourceConsumption::MetricsCollector& ResourceConsumption::MetricsCollector::get(
    OperationContext* opCtx) {
    return getMetricsCollector(opCtx);
}

ResourceConsumption::ScopedMetricsCollector::ScopedMetricsCollector(OperationContext* opCtx,
                                                                    bool commandCollectsMetrics)
    : _opCtx(opCtx) {

    // Nesting is allowed but does nothing. Lower-level ScopedMetricsCollectors should not influence
    // the top-level Collector's behavior.
    auto& metrics = MetricsCollector::get(opCtx);
    _topLevel = !metrics.isInScope();
    if (!_topLevel) {
        return;
    }

    if (!commandCollectsMetrics || !gMeasureOperationResourceConsumption) {
        metrics.beginScopedNotCollecting();
        return;
    }

    metrics.beginScopedCollecting();
}

ResourceConsumption::ScopedMetricsCollector::~ScopedMetricsCollector() {
    if (!_topLevel) {
        return;
    }

    auto& collector = MetricsCollector::get(_opCtx);
    bool wasCollecting = collector.endScopedCollecting();
    if (!wasCollecting) {
        return;
    }

    if (collector.getDbName().empty()) {
        return;
    }

    if (!gAggregateOperationResourceConsumption) {
        return;
    }

    auto& globalResourceConsumption = ResourceConsumption::get(_opCtx);
    globalResourceConsumption.add(collector);
}

ResourceConsumption& ResourceConsumption::get(ServiceContext* svcCtx) {
    return getGlobalResourceConsumption(svcCtx);
}

ResourceConsumption& ResourceConsumption::get(OperationContext* opCtx) {
    return getGlobalResourceConsumption(opCtx->getServiceContext());
}

void ResourceConsumption::add(const MetricsCollector& collector) {
    invariant(!collector.getDbName().empty());
    stdx::unique_lock<Mutex> lk(_mutex);
    _metrics[collector.getDbName()] += collector.getMetrics();
}

ResourceConsumption::MetricsMap ResourceConsumption::getMetrics() const {
    stdx::unique_lock<Mutex> lk(_mutex);
    return _metrics;
}

}  // namespace mongo
