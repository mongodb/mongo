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

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/stdx/memory.h"

#ifdef __linux__
#include "mongo/util/procparser.h"
#endif

namespace mongo {

constexpr auto kSystemMetricsCollector = "systemMetrics";

#ifdef __linux__
static const std::vector<StringData> kCpuKeys{
    "btime", "cpu", "ctxt", "processes", "procs_blocked", "procs_running"};

// Collect all the memory keys by specifying an empty set.
static const std::vector<StringData> kMemKeys{};

/**
 *  Collect metrics from the Linux /proc file system.
 */
class LinuxSystemMetricsCollector final : public FTDCCollectorInterface {
public:
    LinuxSystemMetricsCollector() : _disks(procparser::findPhysicalDisks("/sys/block")) {
        for (const auto& disk : _disks) {
            _disksStringData.emplace_back(disk);
        }
    }

    void collect(OperationContext* txn, BSONObjBuilder& builder) override {
        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("cpu"));
            processStatusErrors(
                procparser::parseProcStatFile("/proc/stat", kCpuKeys, &subObjBuilder),
                &subObjBuilder);
            subObjBuilder.doneFast();
        }

        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("memory"));
            processStatusErrors(
                procparser::parseProcMemInfoFile("/proc/meminfo", kMemKeys, &subObjBuilder),
                &subObjBuilder);
            subObjBuilder.doneFast();
        }

        // Skip the disks section if we could not find any disks.
        // This can happen when we do not have permission to /sys/block for instance.
        if (!_disksStringData.empty()) {
            BSONObjBuilder subObjBuilder(builder.subobjStart("disks"));
            processStatusErrors(procparser::parseProcDiskStatsFile(
                                    "/proc/diskstats", _disksStringData, &subObjBuilder),
                                &subObjBuilder);
            subObjBuilder.doneFast();
        }
    }

    std::string name() const override {
        return kSystemMetricsCollector;
    }

private:
    /**
     * Convert any errors we see into BSON for the user to see in the final FTDC document. It is
     * acceptable for the proc parser to fail, but we do not want to shutdown the FTDC loop because
     * of it. We assume that the BSONBuilder is not corrupt on non-OK Status but nothing else with
     * regards to the final document output.
     */
    static void processStatusErrors(Status s, BSONObjBuilder* builder) {
        if (!s.isOK()) {
            builder->append("error", s.toString());
        }
    }

private:
    // List of physical disks to collect stats from as string from findPhysicalDisks.
    std::vector<std::string> _disks;

    // List of physical disks to collect stats from as StringData to pass to parseProcDiskStatsFile.
    std::vector<StringData> _disksStringData;
};

void installSystemMetricsCollector(FTDCController* controller) {
    controller->addPeriodicCollector(stdx::make_unique<LinuxSystemMetricsCollector>());
}

#else

void installSystemMetricsCollector(FTDCController* controller) {}

#endif

}  // namespace mongo
