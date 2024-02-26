/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <cstdint>
#include <memory>
#include <string>
#include <sys/resource.h>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/ftdc/ftdc_system_stats.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/functional.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/procparser.h"

namespace mongo {

namespace {

static const std::vector<StringData> kCpuKeys{
    "btime"_sd, "cpu"_sd, "ctxt"_sd, "processes"_sd, "procs_blocked"_sd, "procs_running"_sd};

static const std::vector<StringData> kMemKeys{
    "MemAvailable"_sd,
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

static const std::vector<StringData> kNetstatKeys{
    "Tcp:"_sd,
    "Ip:"_sd,
    "TcpExt:"_sd,
    "IpExt:"_sd,
};

static const std::vector<StringData> kVMKeys{
    "balloon_deflate"_sd,
    "balloon_inflate"_sd,
    "nr_mlock"_sd,
    "numa_pages_migrated"_sd,
    "pgfault"_sd,
    "pgmajfault"_sd,
    "pswpin"_sd,
    "pswpout"_sd,
};

// Keys the system stats collector wants to collect out of the /proc/net/sockstat file.
static const std::map<StringData, std::set<StringData>> kSockstatKeys{
    {"sockets"_sd, {"used"_sd}},
    {"TCP"_sd, {"inuse"_sd, "orphan"_sd, "tw"_sd, "alloc"_sd}},
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

    void collect(OperationContext* opCtx, BSONObjBuilder& builder) override {
        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("cpu"_sd));

            // Include the number of cpus to simplify client calculations
            ProcessInfo p;
            subObjBuilder.append("num_logical_cores", static_cast<int>(p.getNumLogicalCores()));
            const auto num_cores_avlbl_to_process = p.getNumCoresAvailableToProcess();
            // Adding the num cores available to process only if API is successful ie. value >=0
            if (num_cores_avlbl_to_process >= 0) {
                subObjBuilder.append("num_cores_available_to_process",
                                     static_cast<int>(num_cores_avlbl_to_process));
            }

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

        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("netstat"_sd));
            processStatusErrors(procparser::parseProcNetstatFile(
                                    kNetstatKeys, "/proc/net/netstat"_sd, &subObjBuilder),
                                &subObjBuilder);
            processStatusErrors(
                procparser::parseProcNetstatFile(kNetstatKeys, "/proc/net/snmp"_sd, &subObjBuilder),
                &subObjBuilder);
            subObjBuilder.doneFast();
        }

        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("sockstat"_sd));
            processStatusErrors(procparser::parseProcSockstatFile(
                                    kSockstatKeys, "/proc/net/sockstat"_sd, &subObjBuilder),
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

        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("mounts"_sd));
            processStatusErrors(
                procparser::parseProcSelfMountStatsFile("/proc/self/mountinfo"_sd, &subObjBuilder),
                &subObjBuilder);
            subObjBuilder.doneFast();
        }

        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("vmstat"_sd));
            processStatusErrors(
                procparser::parseProcVMStatFile("/proc/vmstat"_sd, kVMKeys, &subObjBuilder),
                &subObjBuilder);
            subObjBuilder.doneFast();
        }

        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("files"_sd));
            processStatusErrors(
                procparser::parseProcSysFsFileNrFile("/proc/sys/fs/file-nr"_sd,
                                                     procparser::FileNrKey::kFileHandlesInUse,
                                                     &subObjBuilder),
                &subObjBuilder);
            subObjBuilder.doneFast();
        }

        // TODO SERVER-83707 Remove PSI capture, once T2 uses the ServerStatus section of FTDC
        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("pressure"_sd));
            processStatusErrors(
                procparser::parseProcPressureFile("cpu", "/proc/pressure/cpu"_sd, &subObjBuilder),
                &subObjBuilder);

            processStatusErrors(procparser::parseProcPressureFile(
                                    "memory", "/proc/pressure/memory"_sd, &subObjBuilder),
                                &subObjBuilder);

            processStatusErrors(
                procparser::parseProcPressureFile("io", "/proc/pressure/io"_sd, &subObjBuilder),
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

class SimpleFunctionCollector final : public FTDCCollectorInterface {
public:
    SimpleFunctionCollector(StringData name,
                            unique_function<void(OperationContext*, BSONObjBuilder&)> collectFn)
        : _name(name.toString()), _collectFn(std::move(collectFn)) {}

    void collect(OperationContext* opCtx, BSONObjBuilder& builder) override {
        _collectFn(opCtx, builder);
    }

    std::string name() const override {
        return _name;
    }

private:
    std::string _name;
    unique_function<void(OperationContext*, BSONObjBuilder&)> _collectFn;
};


void collectUlimit(int resource, StringData resourceName, BSONObjBuilder& builder) {

    struct rlimit rlim;

    BSONObjBuilder subObjBuilder(builder.subobjStart(resourceName));

    if (!getrlimit(resource, &rlim)) {
        subObjBuilder.append("soft", static_cast<int64_t>(rlim.rlim_cur));
        subObjBuilder.append("hard", static_cast<int64_t>(rlim.rlim_max));
    } else {
        auto ec = lastSystemError();

        subObjBuilder.append("error", errorMessage(ec));
    }
}

void collectUlimits(OperationContext*, BSONObjBuilder& builder) {
    collectUlimit(RLIMIT_CPU, "cpuTime_secs"_sd, builder);
    collectUlimit(RLIMIT_FSIZE, "fileSize_blocks"_sd, builder);
    collectUlimit(RLIMIT_DATA, "dataSegSize_kb"_sd, builder);
    collectUlimit(RLIMIT_STACK, "stackSize_kb"_sd, builder);
    collectUlimit(RLIMIT_CORE, "coreFileSize_blocks"_sd, builder);
    collectUlimit(RLIMIT_RSS, "residentSize_kb"_sd, builder);
    collectUlimit(RLIMIT_NOFILE, "fileDescriptors"_sd, builder);
    collectUlimit(RLIMIT_AS, "addressSpace_kb"_sd, builder);
    collectUlimit(RLIMIT_NPROC, "processes"_sd, builder);
    collectUlimit(RLIMIT_MEMLOCK, "memLock_kb"_sd, builder);
    collectUlimit(RLIMIT_LOCKS, "fileLocks"_sd, builder);
    collectUlimit(RLIMIT_SIGPENDING, "pendingSignals"_sd, builder);
}

}  // namespace


void installSystemMetricsCollector(FTDCController* controller) {
    controller->addPeriodicCollector(std::make_unique<LinuxSystemMetricsCollector>(),
                                     ClusterRole::None);

    // Total max open files is only collected on rotate, since it changes infrequently
    controller->addOnRotateCollector(
        std::make_unique<SimpleFunctionCollector>(
            "sysMaxOpenFiles",
            [](OperationContext* ctx, BSONObjBuilder& builder) {
                auto status = procparser::parseProcSysFsFileNrFile(
                    "/proc/sys/fs/file-nr", procparser::FileNrKey::kMaxFileHandles, &builder);
                // Handle errors here similarly to system stats.
                if (!status.isOK()) {
                    builder.append("error", status.toString());
                }
            }),
        ClusterRole::None);

    // Collect ULimits settings on rotation.
    controller->addOnRotateCollector(
        std::make_unique<SimpleFunctionCollector>("ulimits", collectUlimits), ClusterRole::None);
}

}  // namespace mongo
