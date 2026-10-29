// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/ftdc/ftdc_system_stats.h"
#include "mongo/logv2/log.h"
#include "mongo/util/perfctr_collect.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC


namespace mongo {

namespace {
using namespace std::literals::string_view_literals;

const std::vector<std::string_view> kCpuCounters = {
    "\\Processor(_Total)\\% Idle Time"sv,
    "\\Processor(_Total)\\% Interrupt Time"sv,
    "\\Processor(_Total)\\% Privileged Time"sv,
    "\\Processor(_Total)\\% Processor Time"sv,
    "\\Processor(_Total)\\% User Time"sv,
    "\\Processor(_Total)\\Interrupts/sec"sv,
    "\\System\\Context Switches/sec"sv,
    "\\System\\Processes"sv,
    "\\System\\Processor Queue Length"sv,
    "\\System\\System Up Time"sv,
    "\\System\\Threads"sv,
};

const std::vector<std::string_view> kMemoryCounters = {
    "\\Memory\\Available Bytes"sv,
    "\\Memory\\Cache Bytes"sv,
    "\\Memory\\Cache Faults/sec"sv,
    "\\Memory\\Committed Bytes"sv,
    "\\Memory\\Commit Limit"sv,
    "\\Memory\\Page Reads/sec"sv,
    "\\Memory\\Page Writes/sec"sv,
    "\\Memory\\Pages Input/sec"sv,
    "\\Memory\\Pages Output/sec"sv,
    "\\Memory\\Pool Nonpaged Bytes"sv,
    "\\Memory\\Pool Paged Bytes"sv,
    "\\Memory\\Pool Paged Resident Bytes"sv,
    "\\Memory\\System Cache Resident Bytes"sv,
    "\\Memory\\System Code Total Bytes"sv,

};

const std::vector<std::string_view> kDiskCounters = {
    "\\PhysicalDisk(*)\\% Disk Read Time"sv,
    "\\PhysicalDisk(*)\\% Disk Write Time"sv,
    "\\PhysicalDisk(*)\\Avg. Disk Read Queue Length"sv,
    "\\PhysicalDisk(*)\\Avg. Disk Write Queue Length"sv,
    "\\PhysicalDisk(*)\\Disk Read Bytes/sec"sv,
    "\\PhysicalDisk(*)\\Disk Write Bytes/sec"sv,
    "\\PhysicalDisk(*)\\Disk Reads/sec"sv,
    "\\PhysicalDisk(*)\\Disk Writes/sec"sv,
    "\\PhysicalDisk(*)\\Current Disk Queue Length"sv,
};


/**
 *  Collect metrics from Windows Performance Counters.
 */
class WindowsSystemMetricsCollector final : public SystemMetricsCollector {
public:
    WindowsSystemMetricsCollector(std::unique_ptr<PerfCounterCollector> collector)
        : _collector(std::move(collector)) {}

    void collect(OperationContext* opCtx, BSONObjBuilder& builder) override {
        processStatusErrors(_collector->collect(&builder), &builder);
    }

private:
    std::unique_ptr<PerfCounterCollector> _collector;
};


StatusWith<std::unique_ptr<PerfCounterCollector>> createCollector() {
    PerfCounterCollection collection;

    Status s = collection.addCountersGroup("cpu", kCpuCounters);
    if (!s.isOK()) {
        return s;
    }

    // TODO: Should we capture the Heap Counters for the current process?
    s = collection.addCountersGroup("memory", kMemoryCounters);
    if (!s.isOK()) {
        return s;
    }

    s = collection.addCountersGroupedByInstanceName("disks", kDiskCounters);
    if (!s.isOK()) {
        return s;
    }

    auto swCollector = PerfCounterCollector::create(std::move(collection));
    if (!swCollector.getStatus().isOK()) {
        return swCollector.getStatus();
    }

    return {std::move(swCollector.getValue())};
}

}  // namespace

void installSystemMetricsCollector(FTDCController* controller) {

    auto swCollector = createCollector();
    if (!swCollector.getStatus().isOK()) {
        LOGV2_WARNING(23718,
                      "Failed to initialize Performance Counters for FTDC",
                      "error"_attr = swCollector.getStatus());
        return;
    }

    controller->addPeriodicCollector(
        std::make_unique<WindowsSystemMetricsCollector>(std::move(swCollector.getValue())));
}

}  // namespace mongo
