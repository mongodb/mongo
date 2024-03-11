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


#include <mutex>
#include <utility>


#include "mongo/bson/bsonelement.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/stats/operation_resource_consumption_gen.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/decorable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResourceConsumption


namespace mongo {
namespace {
const OperationContext::Decoration<ResourceConsumption::MetricsCollector> getMetricsCollector =
    OperationContext::declareDecoration<ResourceConsumption::MetricsCollector>();
const ServiceContext::Decoration<ResourceConsumption> getGlobalResourceConsumption =
    ServiceContext::declareDecoration<ResourceConsumption>();

static const char kCpuNanos[] = "cpuNanos";
static const char kCursorSeeks[] = "cursorSeeks";
static const char kDocBytesRead[] = "docBytesRead";
static const char kDocBytesWritten[] = "docBytesWritten";
static const char kDocUnitsRead[] = "docUnitsRead";
static const char kDocUnitsReturned[] = "docUnitsReturned";
static const char kDocUnitsWritten[] = "docUnitsWritten";
static const char kIdxEntryBytesRead[] = "idxEntryBytesRead";
static const char kIdxEntryBytesWritten[] = "idxEntryBytesWritten";
static const char kIdxEntryUnitsRead[] = "idxEntryUnitsRead";
static const char kIdxEntryUnitsWritten[] = "idxEntryUnitsWritten";
static const char kTotalUnitsWritten[] = "totalUnitsWritten";
static const char kKeysSorted[] = "keysSorted";
static const char kMemUsage[] = "memUsage";
static const char kNumMetrics[] = "numMetrics";
static const char kPrimaryMetrics[] = "primaryMetrics";
static const char kSecondaryMetrics[] = "secondaryMetrics";
static const char kSorterSpills[] = "sorterSpills";

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
    using ServerStatusSection::ServerStatusSection;

    // Do not include this section unless metrics aggregation is enabled. It will not have populated
    // data otherwise.
    bool includeByDefault() const override {
        return ResourceConsumption::isMetricsAggregationEnabled();
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& configElem) const override {
        auto& resourceConsumption = ResourceConsumption::get(opCtx);
        if (!resourceConsumption.isMetricsAggregationEnabled()) {
            return BSONObj();
        }
        BSONObjBuilder builder;
        builder.append(kCpuNanos, durationCount<Nanoseconds>(resourceConsumption.getCpuTime()));

        // The memory usage we report only estimates the amount of memory from the metrics
        // themselves, and does not include the overhead of the map container or its keys.
        auto numMetrics = resourceConsumption.getNumDbMetrics();
        builder.append(
            kMemUsage,
            static_cast<long long>(numMetrics * sizeof(ResourceConsumption::AggregatedMetrics)));
        builder.append(kNumMetrics, static_cast<long long>(numMetrics));
        return builder.obj();
    }
};

auto& resourceConsumptionSSM =
    *ServerStatusSectionBuilder<ResourceConsumptionSSS>("resourceConsumption");

}  // namespace

bool ResourceConsumption::isMetricsCollectionEnabled() {
    return isMetricsProfilingEnabled() || isMetricsAggregationEnabled();
}

bool ResourceConsumption::isMetricsProfilingEnabled() {
    return gProfileOperationResourceConsumptionMetrics;
}

bool ResourceConsumption::isMetricsAggregationEnabled() {
    return gAggregateOperationResourceConsumptionMetrics;
}

ResourceConsumption::MetricsCollector& ResourceConsumption::MetricsCollector::get(
    OperationContext* opCtx) {
    return getMetricsCollector(opCtx);
}

void ResourceConsumption::UnitCounter::observeOne(int64_t datumBytes) {
    _units += std::ceil(datumBytes / static_cast<float>(unitSize()));
    _bytes += datumBytes;
}

void ResourceConsumption::TotalUnitWriteCounter::observeOneDocument(int64_t datumBytes) {
    // If we have accumulated document bytes, calculate units along with any past index bytes.
    // Accumulate the current document bytes for use in a later unit calculation.
    if (_accumulatedDocumentBytes > 0) {
        _units += std::ceil((_accumulatedIndexBytes + _accumulatedDocumentBytes) /
                            static_cast<float>(unitSize()));
        _accumulatedIndexBytes = 0;
        _accumulatedDocumentBytes = datumBytes;
        return;
    }

    // If we have accumulated index bytes, associate them with the current document and calculate
    // units.
    if (_accumulatedIndexBytes > 0) {
        _units += std::ceil((_accumulatedIndexBytes + datumBytes) / static_cast<float>(unitSize()));
        _accumulatedIndexBytes = 0;
        return;
    }

    // Nothing has yet accumulated; accumulate this document for use in a later unit calculation.
    _accumulatedDocumentBytes = datumBytes;
}

void ResourceConsumption::TotalUnitWriteCounter::observeOneIndexEntry(int64_t datumBytes) {
    _accumulatedIndexBytes += datumBytes;
}

int ResourceConsumption::DocumentUnitCounter::unitSize() const {
    return gDocumentUnitSizeBytes;
}

int ResourceConsumption::IdxEntryUnitCounter::unitSize() const {
    return gIndexEntryUnitSizeBytes;
}

int ResourceConsumption::TotalUnitWriteCounter::unitSize() const {
    return gTotalUnitWriteSizeBytes;
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
    builder->appendNumber(kTotalUnitsWritten, totalWritten.units());
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

BSONObj ResourceConsumption::OperationMetrics::toBson() const {
    BSONObjBuilder builder;
    toBson(&builder);
    return builder.obj();
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
    appendNonZeroMetric(builder, kTotalUnitsWritten, writeMetrics.totalWritten.units());
}

void ResourceConsumption::MetricsCollector::_incrementOneDocRead(StringData uri,
                                                                 int64_t docBytesRead) {
    _metrics.readMetrics.docsRead.observeOne(docBytesRead);
    LOGV2_DEBUG(6523900,
                2,
                "ResourceConsumption::MetricsCollector::incrementOneDocRead",
                "uri"_attr = uri,
                "bytes"_attr = docBytesRead);
}

void ResourceConsumption::MetricsCollector::_incrementOneIdxEntryRead(StringData uri,
                                                                      int64_t bytesRead) {
    _metrics.readMetrics.idxEntriesRead.observeOne(bytesRead);

    LOGV2_DEBUG(6523901,
                2,
                "ResourceConsumption::MetricsCollector::incrementOneIdxEntryRead",
                "uri"_attr = uri,
                "bytes"_attr = bytesRead);
}

void ResourceConsumption::MetricsCollector::_incrementKeysSorted(int64_t keysSorted) {
    _metrics.readMetrics.keysSorted += keysSorted;
    LOGV2_DEBUG(6523902,
                2,
                "ResourceConsumption::MetricsCollector::incrementKeysSorted",
                "keysSorted"_attr = keysSorted);
}

void ResourceConsumption::MetricsCollector::_incrementSorterSpills(int64_t spills) {
    _metrics.readMetrics.sorterSpills += spills;
    LOGV2_DEBUG(6523903,
                2,
                "ResourceConsumption::MetricsCollector::incrementSorterSpills",
                "spills"_attr = spills);
}

void ResourceConsumption::MetricsCollector::_incrementDocUnitsReturned(
    StringData ns, DocumentUnitCounter docUnits) {
    _metrics.readMetrics.docsReturned += docUnits;
    LOGV2_DEBUG(6523904,
                2,
                "ResourceConsumption::MetricsCollector::incrementDocUnitsReturned",
                "ns"_attr = ns,
                "docUnits"_attr = docUnits.units());
}

void ResourceConsumption::MetricsCollector::_incrementOneDocWritten(StringData uri,
                                                                    int64_t bytesWritten) {
    _metrics.writeMetrics.docsWritten.observeOne(bytesWritten);
    _metrics.writeMetrics.totalWritten.observeOneDocument(bytesWritten);
    LOGV2_DEBUG(6523905,
                2,
                "ResourceConsumption::MetricsCollector::incrementOneDocWritten",
                "uri"_attr = uri,
                "bytesWritten"_attr = bytesWritten);
}

void ResourceConsumption::MetricsCollector::_incrementOneIdxEntryWritten(StringData uri,
                                                                         int64_t bytesWritten) {
    _metrics.writeMetrics.idxEntriesWritten.observeOne(bytesWritten);
    _metrics.writeMetrics.totalWritten.observeOneIndexEntry(bytesWritten);
    LOGV2_DEBUG(6523906,
                2,
                "ResourceConsumption::MetricsCollector::incrementOneIdxEntryWritten",
                "uri"_attr = uri,
                "bytesWritten"_attr = bytesWritten);
}

void ResourceConsumption::MetricsCollector::_incrementOneCursorSeek(StringData uri) {
    _metrics.readMetrics.cursorSeeks++;
    LOGV2_DEBUG(6523907,
                2,
                "ResourceConsumption::MetricsCollector::incrementOneCursorSeek",
                "uri"_attr = uri);
}

void ResourceConsumption::MetricsCollector::beginScopedCollecting(OperationContext* opCtx,
                                                                  const DatabaseName& dbName) {
    invariant(!isInScope());
    _dbName = dbName;
    _collecting |= (ScopedCollectionState::kInScope | ScopedCollectionState::kCollecting);
    _hasCollectedMetrics = true;

    // We must clear the metrics here to ensure we do not accumulate metrics from previous scoped
    // collections. Note that we can't clear metrics in endScopedCollecting() because consumers
    // expect metrics to be available after a scoped collection period has ended.
    _metrics = {};

    // The OperationCPUTimers may be nullptr on unsupported systems.
    if (auto timers = OperationCPUTimers::get(opCtx)) {
        _metrics.cpuTimer = timers->makeTimer();
        _metrics.cpuTimer->start();
    }
}

bool ResourceConsumption::MetricsCollector::endScopedCollecting() {
    bool wasCollecting = isCollecting();
    if (wasCollecting && _metrics.cpuTimer) {
        _metrics.cpuTimer->stop();
    }
    _collecting &= ~(ScopedCollectionState::kInScope | ScopedCollectionState::kCollecting);
    return wasCollecting;
}

ResourceConsumption::ScopedMetricsCollector::ScopedMetricsCollector(OperationContext* opCtx,
                                                                    const DatabaseName& dbName,
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
                                const DatabaseName& dbName,
                                const OperationMetrics& metrics) {
    invariant(!dbName.isEmpty());

    LOGV2_DEBUG(7527700,
                1,
                "ResourceConsumption::merge",
                "dbName"_attr = dbName,
                "metrics"_attr = metrics.toBson());

    // All metrics over the duration of this operation will be attributed to the current state, even
    // if it ran accross state transitions.
    // The RSTL is normally required to check the replication state, but callers may not always be
    // holding it. Since we need to attribute this metric to some replication state, and an
    // inconsistent state is not impactful for the purposes of metrics collection, perform a
    // best-effort check so that we can record metrics for this operation.
    auto isPrimary = repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase_UNSAFE(
        opCtx, DatabaseName::kAdmin);

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
    const auto& dbNameStr = dbName.toStringForResourceId();
    stdx::lock_guard<Mutex> lk(_mutex);
    _dbMetrics[dbNameStr] += newMetrics;
    _cpuTime += newMetrics.cpuNanos;
}

ResourceConsumption::MetricsMap ResourceConsumption::getDbMetrics() const {
    stdx::lock_guard<Mutex> lk(_mutex);
    return _dbMetrics;
}

int64_t ResourceConsumption::getNumDbMetrics() const {
    stdx::lock_guard<Mutex> lk(_mutex);
    return _dbMetrics.size();
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
