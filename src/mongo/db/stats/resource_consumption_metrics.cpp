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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include <cmath>

#include "mongo/db/stats/resource_consumption_metrics.h"

#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/stats/operation_resource_consumption_gen.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {
const OperationContext::Decoration<ResourceConsumption::MetricsCollector> getMetricsCollector =
    OperationContext::declareDecoration<ResourceConsumption::MetricsCollector>();
const ServiceContext::Decoration<ResourceConsumption> getGlobalResourceConsumption =
    ServiceContext::declareDecoration<ResourceConsumption>();

static const char kPrimaryMetrics[] = "primaryMetrics";
static const char kSecondaryMetrics[] = "secondaryMetrics";
static const char kDocBytesRead[] = "docBytesRead";
static const char kDocUnitsRead[] = "docUnitsRead";
static const char kIdxEntryBytesRead[] = "idxEntryBytesRead";
static const char kIdxEntryUnitsRead[] = "idxEntryUnitsRead";
static const char kKeysSorted[] = "keysSorted";
static const char kCpuNanos[] = "cpuNanos";
static const char kDocBytesWritten[] = "docBytesWritten";
static const char kDocUnitsWritten[] = "docUnitsWritten";
static const char kIdxEntryBytesWritten[] = "idxEntryBytesWritten";
static const char kIdxEntryUnitsWritten[] = "idxEntryUnitsWritten";
static const char kDocUnitsReturned[] = "docUnitsReturned";

inline void appendNonZeroMetric(BSONObjBuilder* builder, const char* name, long long value) {
    if (value != 0) {
        builder->append(name, value);
    }
}
}  // namespace

bool ResourceConsumption::isMetricsCollectionEnabled() {
    return gMeasureOperationResourceConsumption.isEnabledAndIgnoreFCV();
}

bool ResourceConsumption::isMetricsAggregationEnabled() {
    return gAggregateOperationResourceConsumptionMetrics;
}

ResourceConsumption::ResourceConsumption() {
    if (gAggregateOperationResourceConsumptionMetrics &&
        !gMeasureOperationResourceConsumption.isEnabledAndIgnoreFCV()) {
        LOGV2_FATAL_NOTRACE(
            5091600,
            "measureOperationResourceConsumption feature flag must be enabled to use "
            "aggregateOperationResourceConsumptionMetrics");
    }
}

ResourceConsumption::MetricsCollector& ResourceConsumption::MetricsCollector::get(
    OperationContext* opCtx) {
    return getMetricsCollector(opCtx);
}

void ResourceConsumption::ReadMetrics::toBson(BSONObjBuilder* builder) const {
    builder->appendNumber(kDocBytesRead, docBytesRead);
    builder->appendNumber(kDocUnitsRead, docUnitsRead);
    builder->appendNumber(kIdxEntryBytesRead, idxEntryBytesRead);
    builder->appendNumber(kIdxEntryUnitsRead, idxEntryUnitsRead);
    builder->appendNumber(kKeysSorted, keysSorted);
    builder->appendNumber(kDocUnitsReturned, docUnitsReturned);
}

void ResourceConsumption::WriteMetrics::toBson(BSONObjBuilder* builder) const {
    builder->appendNumber(kDocBytesWritten, docBytesWritten);
    builder->appendNumber(kDocUnitsWritten, docUnitsWritten);
    builder->appendNumber(kIdxEntryBytesWritten, idxEntryBytesWritten);
    builder->appendNumber(kIdxEntryUnitsWritten, idxEntryUnitsWritten);
}

void ResourceConsumption::AggregatedMetrics::toBson(BSONObjBuilder* builder) const {
    {
        BSONObjBuilder primaryBuilder = builder->subobjStart(kPrimaryMetrics);
        primaryReadMetrics.toBson(&primaryBuilder);
        primaryBuilder.done();
    }

    {
        BSONObjBuilder secondaryBuilder = builder->subobjStart(kSecondaryMetrics);
        secondaryReadMetrics.toBson(&secondaryBuilder);
        secondaryBuilder.done();
    }

    writeMetrics.toBson(builder);
    builder->appendNumber(kCpuNanos, durationCount<Nanoseconds>(cpuNanos));
}

void ResourceConsumption::OperationMetrics::toBson(BSONObjBuilder* builder) const {
    readMetrics.toBson(builder);
    writeMetrics.toBson(builder);
    if (cpuTimer) {
        builder->appendNumber(kCpuNanos, durationCount<Nanoseconds>(cpuTimer->getElapsed()));
    }
}

void ResourceConsumption::OperationMetrics::toBsonNonZeroFields(BSONObjBuilder* builder) const {
    appendNonZeroMetric(builder, kDocBytesRead, readMetrics.docBytesRead);
    appendNonZeroMetric(builder, kDocUnitsRead, readMetrics.docUnitsRead);
    appendNonZeroMetric(builder, kIdxEntryBytesRead, readMetrics.idxEntryBytesRead);
    appendNonZeroMetric(builder, kIdxEntryUnitsRead, readMetrics.idxEntryUnitsRead);
    appendNonZeroMetric(builder, kKeysSorted, readMetrics.keysSorted);
    appendNonZeroMetric(builder, kDocUnitsReturned, readMetrics.docUnitsReturned);

    if (cpuTimer) {
        appendNonZeroMetric(builder, kCpuNanos, durationCount<Nanoseconds>(cpuTimer->getElapsed()));
    }
    appendNonZeroMetric(builder, kDocBytesWritten, writeMetrics.docBytesWritten);
    appendNonZeroMetric(builder, kDocUnitsWritten, writeMetrics.docUnitsWritten);
    appendNonZeroMetric(builder, kIdxEntryBytesWritten, writeMetrics.idxEntryBytesWritten);
    appendNonZeroMetric(builder, kIdxEntryUnitsWritten, writeMetrics.idxEntryUnitsWritten);
}

template <typename Func>
inline void ResourceConsumption::MetricsCollector::_doIfCollecting(Func&& func) {
    if (!isCollecting()) {
        return;
    }
    func();
}

void ResourceConsumption::MetricsCollector::incrementOneDocRead(OperationContext* opCtx,
                                                                size_t docBytesRead) {
    _doIfCollecting([&]() {
        size_t docUnits = std::ceil(docBytesRead / static_cast<float>(gDocumentUnitSizeBytes));
        _metrics.readMetrics.docBytesRead += docBytesRead;
        _metrics.readMetrics.docUnitsRead += docUnits;
    });
}

void ResourceConsumption::MetricsCollector::incrementOneIdxEntryRead(OperationContext* opCtx,
                                                                     size_t bytesRead) {
    _doIfCollecting([&]() {
        size_t units = std::ceil(bytesRead / static_cast<float>(gIndexEntryUnitSizeBytes));
        _metrics.readMetrics.idxEntryBytesRead += bytesRead;
        _metrics.readMetrics.idxEntryUnitsRead += units;
    });
}

void ResourceConsumption::MetricsCollector::incrementKeysSorted(OperationContext* opCtx,
                                                                size_t keysSorted) {
    _doIfCollecting([&]() { _metrics.readMetrics.keysSorted += keysSorted; });
}

void ResourceConsumption::MetricsCollector::incrementDocUnitsReturned(OperationContext* opCtx,
                                                                      size_t returned) {
    _doIfCollecting([&]() { _metrics.readMetrics.docUnitsReturned += returned; });
}

void ResourceConsumption::MetricsCollector::incrementOneDocWritten(size_t bytesWritten) {
    _doIfCollecting([&] {
        size_t docUnits = std::ceil(bytesWritten / static_cast<float>(gDocumentUnitSizeBytes));
        _metrics.writeMetrics.docBytesWritten += bytesWritten;
        _metrics.writeMetrics.docUnitsWritten += docUnits;
    });
}

void ResourceConsumption::MetricsCollector::incrementOneIdxEntryWritten(size_t bytesWritten) {
    _doIfCollecting([&] {
        size_t idxUnits = std::ceil(bytesWritten / static_cast<float>(gIndexEntryUnitSizeBytes));
        _metrics.writeMetrics.idxEntryBytesWritten += bytesWritten;
        _metrics.writeMetrics.idxEntryUnitsWritten += idxUnits;
    });
}

void ResourceConsumption::MetricsCollector::beginScopedCollecting(OperationContext* opCtx,
                                                                  const std::string& dbName) {
    invariant(!isInScope());
    _dbName = dbName;
    _collecting = ScopedCollectionState::kInScopeCollecting;
    _hasCollectedMetrics = true;

    // The OperationCPUTimer may be nullptr on unsupported systems.
    _metrics.cpuTimer = OperationCPUTimer::get(opCtx);
    if (_metrics.cpuTimer) {
        _metrics.cpuTimer->start();
    }
}

bool ResourceConsumption::MetricsCollector::endScopedCollecting() {
    bool wasCollecting = isCollecting();
    if (wasCollecting && _metrics.cpuTimer) {
        _metrics.cpuTimer->stop();
    }
    _collecting = ScopedCollectionState::kInactive;
    return wasCollecting;
}

ResourceConsumption::ScopedMetricsCollector::ScopedMetricsCollector(OperationContext* opCtx,
                                                                    const std::string& dbName,
                                                                    bool commandCollectsMetrics)
    : _opCtx(opCtx) {

    // Nesting is allowed but does nothing. Lower-level ScopedMetricsCollectors should not influence
    // the top-level Collector's behavior.
    auto& metrics = MetricsCollector::get(opCtx);
    _topLevel = !metrics.isInScope();
    if (!_topLevel) {
        return;
    }

    if (!commandCollectsMetrics || !shouldCollectMetricsForDatabase(dbName) ||
        !isMetricsCollectionEnabled()) {
        metrics.beginScopedNotCollecting();
        return;
    }

    metrics.beginScopedCollecting(opCtx, dbName);
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

    if (!isMetricsAggregationEnabled()) {
        return;
    }

    auto& globalResourceConsumption = ResourceConsumption::get(_opCtx);
    globalResourceConsumption.merge(_opCtx, collector.getDbName(), collector.getMetrics());
}

ResourceConsumption& ResourceConsumption::get(ServiceContext* svcCtx) {
    return getGlobalResourceConsumption(svcCtx);
}

ResourceConsumption& ResourceConsumption::get(OperationContext* opCtx) {
    return getGlobalResourceConsumption(opCtx->getServiceContext());
}

void ResourceConsumption::merge(OperationContext* opCtx,
                                const std::string& dbName,
                                const OperationMetrics& metrics) {
    invariant(!dbName.empty());

    // All metrics over the duration of this operation will be attributed to the current state, even
    // if it ran accross state transitions.
    // The RSTL is normally required to check the replication state, but callers may not always be
    // holding it. Since we need to attribute this metric to some replication state, and an
    // inconsistent state is not impactful for the purposes of metrics collection, perform a
    // best-effort check so that we can record metrics for this operation.
    auto isPrimary = repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase_UNSAFE(
        opCtx, NamespaceString::kAdminDb);

    // Add all metrics into the the globally-aggregated metrics.
    stdx::unique_lock<Mutex> lk(_mutex);
    auto& elem = _metrics[dbName];

    if (isPrimary) {
        elem.primaryReadMetrics += metrics.readMetrics;
    } else {
        elem.secondaryReadMetrics += metrics.readMetrics;
    }
    elem.writeMetrics += metrics.writeMetrics;
    if (metrics.cpuTimer) {
        elem.cpuNanos += metrics.cpuTimer->getElapsed();
    }
}

ResourceConsumption::MetricsMap ResourceConsumption::getMetrics() const {
    stdx::unique_lock<Mutex> lk(_mutex);
    return _metrics;
}

ResourceConsumption::MetricsMap ResourceConsumption::getAndClearMetrics() {
    stdx::unique_lock<Mutex> lk(_mutex);
    MetricsMap newMap;
    _metrics.swap(newMap);
    return newMap;
}

}  // namespace mongo
