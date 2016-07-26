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
#include "mongo/util/procparser.h"

namespace mongo {

namespace {

static const std::vector<StringData> kCpuKeys{
    "btime"_sd, "cpu"_sd, "ctxt"_sd, "processes"_sd, "procs_blocked"_sd, "procs_running"_sd};

static const std::vector<StringData> kMemKeys{
    "MemTotal"_sd,
    "MemFree"_sd,
    "Cached"_sd,
    "Dirty"_sd,
    "Buffers"_sd,
    "SwapTotal"_sd,
    "SwapCached"_sd,
    "SwapFree"_sd,
    "Active"_sd,
    "Inactive"_sd,
    "Active(anon)"_sd,
    "Inactive(anon)"_sd,
    "Active(file)"_sd,
    "Inactive(file)"_sd,
};

/**
 *  Collect metrics from the Linux /proc file system.
 */
class LinuxSystemMetricsCollector final : public SystemMetricsCollector {
public:
    LinuxSystemMetricsCollector() : _disks(procparser::findPhysicalDisks("/sys/block"_sd)) {
        for (const auto& disk : _disks) {
            _disksStringData.emplace_back(disk);
        }
    }

    void collect(OperationContext* txn, BSONObjBuilder& builder) override {
        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("cpu"_sd));
            processStatusErrors(
                procparser::parseProcStatFile("/proc/stat"_sd, kCpuKeys, &subObjBuilder),
                &subObjBuilder);
            subObjBuilder.doneFast();
        }

        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("memory"_sd));
            processStatusErrors(
                procparser::parseProcMemInfoFile("/proc/meminfo"_sd, kMemKeys, &subObjBuilder),
                &subObjBuilder);
            subObjBuilder.doneFast();
        }

        // Skip the disks section if we could not find any disks.
        // This can happen when we do not have permission to /sys/block for instance.
        if (!_disksStringData.empty()) {
            BSONObjBuilder subObjBuilder(builder.subobjStart("disks"_sd));
            processStatusErrors(procparser::parseProcDiskStatsFile(
                                    "/proc/diskstats"_sd, _disksStringData, &subObjBuilder),
                                &subObjBuilder);
            subObjBuilder.doneFast();
        }
    }

private:
    // List of physical disks to collect stats from as string from findPhysicalDisks.
    std::vector<std::string> _disks;

    // List of physical disks to collect stats from as StringData to pass to parseProcDiskStatsFile.
    std::vector<StringData> _disksStringData;
};

}  // namespace

void installSystemMetricsCollector(FTDCController* controller) {
    controller->addPeriodicCollector(stdx::make_unique<LinuxSystemMetricsCollector>());
}

}  // namespace mongo
