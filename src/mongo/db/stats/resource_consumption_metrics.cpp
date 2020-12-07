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

#include "mongo/db/commands/server_status.h"
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
static const char kSorterSpills[] = "sorterSpills";
static const char kCpuNanos[] = "cpuNanos";
static const char kDocBytesWritten[] = "docBytesWritten";
static const char kDocUnitsWritten[] = "docUnitsWritten";
static const char kIdxEntryBytesWritten[] = "idxEntryBytesWritten";
static const char kIdxEntryUnitsWritten[] = "idxEntryUnitsWritten";
static const char kDocUnitsReturned[] = "docUnitsReturned";
static const char kCursorSeeks[] = "cursorSeeks";

inline void appendNonZeroMetric(BSONObjBuilder* builder, const char* name, long long value) {
    if (value != 0) {
        builder->append(name, value);
    }
}

/**
 * Reports globally-aggregated CPU time spent by user operations and a specific set of commands.
 */
class ResourceConsumptionSSS : public ServerStatusSection {
public:
    ResourceConsumptionSSS() : ServerStatusSection("resourceConsumption") {}

    // Do not include this metric by default. It is likely to be misleading for diagnostic purposes
    // because it does not include the CPU time for every operation or every command.
    bool includeByDefault() const override {
        return false;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& configElem) const override {
        auto& resourceConsumption = ResourceConsumption::get(opCtx);
        if (!resourceConsumption.isMetricsAggregationEnabled()) {
            return BSONObj();
        }
        BSONObjBuilder builder;
        builder.append(kCpuNanos, durationCount<Nanoseconds>(resourceConsumption.getCpuTime()));
        return builder.obj();
    }
} resourceConsumptionMetricSSM;

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

void ResourceConsumption::UnitCounter::observeOne(size_t datumBytes) {
    _units += std::ceil(datumBytes / static_cast<float>(unitSize()));
    _bytes += datumBytes;
}

int ResourceConsumption::DocumentUnitCounter::unitSize() const {
    return gDocumentUnitSizeBytes;
}

int ResourceConsumption::IdxEntryUnitCounter::unitSize() const {
    return gIndexEntryUnitSizeBytes;
}

void ResourceConsumption::ReadMetrics::toBson(BSONObjBuilder* builder) const {
    builder->appendNumber(kDocBytesRead, docsRead.bytes());
    builder->appendNumber(kDocUnitsRead, docsRead.units());
    builder->appendNumber(kIdxEntryBytesRead, idxEntriesRead.bytes());
    builder->appendNumber(kIdxEntryUnitsRead, idxEntriesRead.units());
    builder->appendNumber(kKeysSorted, keysSorted);
    builder->appendNumber(kSorterSpills, sorterSpills);
    builder->appendNumber(kDocUnitsReturned, docsReturned.units());
    builder->appendNumber(kCursorSeeks, cursorSeeks);
}

void ResourceConsumption::WriteMetrics::toBson(BSONObjBuilder* builder) const {
    builder->appendNumber(kDocBytesWritten, docsWritten.bytes());
    builder->appendNumber(kDocUnitsWritten, docsWritten.units());
    builder->appendNumber(kIdxEntryBytesWritten, idxEntriesWritten.bytes());
    builder->appendNumber(kIdxEntryUnitsWritten, idxEntriesWritten.units());
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
    appendNonZeroMetric(builder, kDocBytesRead, readMetrics.docsRead.bytes());
    appendNonZeroMetric(builder, kDocUnitsRead, readMetrics.docsRead.units());
    appendNonZeroMetric(builder, kIdxEntryBytesRead, readMetrics.idxEntriesRead.bytes());
    appendNonZeroMetric(builder, kIdxEntryUnitsRead, readMetrics.idxEntriesRead.units());
    appendNonZeroMetric(builder, kKeysSorted, readMetrics.keysSorted);
    appendNonZeroMetric(builder, kSorterSpills, readMetrics.sorterSpills);
    appendNonZeroMetric(builder, kDocUnitsReturned, readMetrics.docsReturned.units());
    appendNonZeroMetric(builder, kCursorSeeks, readMetrics.cursorSeeks);

    if (cpuTimer) {
        appendNonZeroMetric(builder, kCpuNanos, durationCount<Nanoseconds>(cpuTimer->getElapsed()));
    }
    appendNonZeroMetric(builder, kDocBytesWritten, writeMetrics.docsWritten.bytes());
    appendNonZeroMetric(builder, kDocUnitsWritten, writeMetrics.docsWritten.units());
    appendNonZeroMetric(builder, kIdxEntryBytesWritten, writeMetrics.idxEntriesWritten.bytes());
    appendNonZeroMetric(builder, kIdxEntryUnitsWritten, writeMetrics.idxEntriesWritten.units());
}

template <typename Func>
inline void ResourceConsumption::MetricsCollector::_doIfCollecting(Func&& func) {
    if (!isCollecting()) {
        return;
    }
    func();
}

void ResourceConsumption::MetricsCollector::incrementOneDocRead(size_t docBytesRead) {
    _doIfCollecting([&]() { _metrics.readMetrics.docsRead.observeOne(docBytesRead); });
}

void ResourceConsumption::MetricsCollector::incrementOneIdxEntryRead(size_t bytesRead) {
    _doIfCollecting([&]() { _metrics.readMetrics.idxEntriesRead.observeOne(bytesRead); });
}

void ResourceConsumption::MetricsCollector::incrementKeysSorted(size_t keysSorted) {
    _doIfCollecting([&]() { _metrics.readMetrics.keysSorted += keysSorted; });
}

void ResourceConsumption::MetricsCollector::incrementSorterSpills(size_t spills) {
    _doIfCollecting([&]() { _metrics.readMetrics.sorterSpills += spills; });
}

void ResourceConsumption::MetricsCollector::incrementDocUnitsReturned(
    DocumentUnitCounter docUnits) {
    _doIfCollecting([&]() { _metrics.readMetrics.docsReturned += docUnits; });
}

void ResourceConsumption::MetricsCollector::incrementOneDocWritten(size_t bytesWritten) {
    _doIfCollecting([&] { _metrics.writeMetrics.docsWritten.observeOne(bytesWritten); });
}

void ResourceConsumption::MetricsCollector::incrementOneIdxEntryWritten(size_t bytesWritten) {
    _doIfCollecting([&] { _metrics.writeMetrics.idxEntriesWritten.observeOne(bytesWritten); });
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

void ResourceConsumption::MetricsCollector::incrementOneCursorSeek() {
    _doIfCollecting([&] { _metrics.readMetrics.cursorSeeks++; });
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

    AggregatedMetrics newMetrics;
    if (isPrimary) {
        newMetrics.primaryReadMetrics = metrics.readMetrics;
    } else {
        newMetrics.secondaryReadMetrics = metrics.readMetrics;
    }
    newMetrics.writeMetrics = metrics.writeMetrics;
    if (metrics.cpuTimer) {
        newMetrics.cpuNanos = metrics.cpuTimer->getElapsed();
    }

    // Add all metrics into the the globally-aggregated metrics.
    stdx::lock_guard<Mutex> lk(_mutex);
    _dbMetrics[dbName] += newMetrics;
    _cpuTime += newMetrics.cpuNanos;
}

ResourceConsumption::MetricsMap ResourceConsumption::getDbMetrics() const {
    stdx::lock_guard<Mutex> lk(_mutex);
    return _dbMetrics;
}

ResourceConsumption::MetricsMap ResourceConsumption::getAndClearDbMetrics() {
    stdx::lock_guard<Mutex> lk(_mutex);
    MetricsMap newMap;
    _dbMetrics.swap(newMap);
    return newMap;
}

Nanoseconds ResourceConsumption::getCpuTime() const {
    stdx::lock_guard<Mutex> lk(_mutex);
    return _cpuTime;
}

Nanoseconds ResourceConsumption::getAndClearCpuTime() {
    stdx::lock_guard<Mutex> lk(_mutex);
    return std::exchange(_cpuTime, {});
}
}  // namespace mongo
