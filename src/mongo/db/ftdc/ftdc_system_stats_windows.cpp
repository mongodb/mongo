/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kFTDC

#include "mongo/platform/basic.h"

#include "mongo/db/ftdc/ftdc_system_stats.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/perfctr_collect.h"

namespace mongo {

namespace {

const std::vector<StringData> kCpuCounters = {
    "\\Processor(_Total)\\% Idle Time"_sd,
    "\\Processor(_Total)\\% Interrupt Time"_sd,
    "\\Processor(_Total)\\% Privileged Time"_sd,
    "\\Processor(_Total)\\% Processor Time"_sd,
    "\\Processor(_Total)\\% User Time"_sd,
    "\\Processor(_Total)\\Interrupts/sec"_sd,
    "\\System\\Context Switches/sec"_sd,
    "\\System\\Processes"_sd,
    "\\System\\Processor Queue Length"_sd,
    "\\System\\System Up Time"_sd,
    "\\System\\Threads"_sd,
};

const std::vector<StringData> kMemoryCounters = {
    "\\Memory\\Available Bytes"_sd,
    "\\Memory\\Cache Bytes"_sd,
    "\\Memory\\Cache Faults/sec"_sd,
    "\\Memory\\Committed Bytes"_sd,
    "\\Memory\\Commit Limit"_sd,
    "\\Memory\\Page Reads/sec"_sd,
    "\\Memory\\Page Writes/sec"_sd,
    "\\Memory\\Pages Input/sec"_sd,
    "\\Memory\\Pages Output/sec"_sd,
    "\\Memory\\Pool Nonpaged Bytes"_sd,
    "\\Memory\\Pool Paged Bytes"_sd,
    "\\Memory\\Pool Paged Resident Bytes"_sd,
    "\\Memory\\System Cache Resident Bytes"_sd,
    "\\Memory\\System Code Total Bytes"_sd,

};

const std::vector<StringData> kDiskCounters = {
    "\\PhysicalDisk(*)\\% Disk Read Time"_sd,
    "\\PhysicalDisk(*)\\% Disk Write Time"_sd,
    "\\PhysicalDisk(*)\\Avg. Disk Read Queue Length"_sd,
    "\\PhysicalDisk(*)\\Avg. Disk Write Queue Length"_sd,
    "\\PhysicalDisk(*)\\Disk Read Bytes/sec"_sd,
    "\\PhysicalDisk(*)\\Disk Write Bytes/sec"_sd,
    "\\PhysicalDisk(*)\\Disk Reads/sec"_sd,
    "\\PhysicalDisk(*)\\Disk Writes/sec"_sd,
    "\\PhysicalDisk(*)\\Current Disk Queue Length"_sd,
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
        warning() << "Failed to initialize Performance Counters for FTDC: "
                  << swCollector.getStatus();
        return;
    }

    controller->addPeriodicCollector(
        stdx::make_unique<WindowsSystemMetricsCollector>(std::move(swCollector.getValue())));
}

}  // namespace mongo
