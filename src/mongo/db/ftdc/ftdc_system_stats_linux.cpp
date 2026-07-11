// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/ftdc/ftdc_system_stats.h"
#include "mongo/logv2/log.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/functional.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/procparser.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ifaddrs.h>

#include <linux/ethtool.h>
#include <linux/if.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;

static const std::vector<std::string_view> kCpuKeys{
    "btime"sv, "cpu"sv, "ctxt"sv, "processes"sv, "procs_blocked"sv, "procs_running"sv};

static const std::vector<std::string_view> kMemKeys{
    "MemAvailable"sv,
    "MemTotal"sv,
    "MemFree"sv,
    "Cached"sv,
    "Dirty"sv,
    "Buffers"sv,
    "SwapTotal"sv,
    "SwapCached"sv,
    "SwapFree"sv,
    "Active"sv,
    "Inactive"sv,
    "Active(anon)"sv,
    "Inactive(anon)"sv,
    "Active(file)"sv,
    "Inactive(file)"sv,
    "AnonHugePages"sv,
};

static const std::vector<std::string_view> kNetstatKeys{
    "Tcp:"sv,
    "Ip:"sv,
    "TcpExt:"sv,
    "IpExt:"sv,
};

static const std::vector<std::string_view> kVMKeys{
    "balloon_deflate"sv,
    "balloon_inflate"sv,
    "nr_mlock"sv,
    "numa_pages_migrated"sv,
    "pgfault"sv,
    "pgmajfault"sv,
    "pswpin"sv,
    "pswpout"sv,
    "nr_anon_transparent_hugepages"sv,
    "thp_fault_alloc"sv,
    "thp_collapse_alloc"sv,
    "thp_fault_fallback"sv,
    "thp_swpout"sv,
};

// Keys the system stats collector wants to collect out of the /proc/net/sockstat file.
static const std::map<std::string_view, std::set<std::string_view>> kSockstatKeys{
    {"sockets"sv, {"used"sv}},
    {"TCP"sv, {"inuse"sv, "orphan"sv, "tw"sv, "alloc"sv}},
};

/**
 * Class to gather NIC stats by emulating ethtool -S functionality by using the ioctl SIOCETHTOOL.
 */
class EthTool {
public:
    static std::unique_ptr<EthTool> create(std::string_view interface) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd == -1) {
            auto ec = lastPosixError();
            LOGV2_WARNING(
                10985539, "Ethtool socket allocation failed", "error"_attr = errorMessage(ec));
            return nullptr;
        }

        auto ethtool = std::unique_ptr<EthTool>(new EthTool(interface, fd));
        auto drvinfo = ethtool->get_info();

        // Some Linux interfaces cannot be found by ethtool IOCTL.
        // Some Linux interfaces have no stats (i.e. the "bridge" driver used by containers).
        if (!drvinfo.has_value() || drvinfo->n_stats == 0) {
            LOGV2_WARNING(10985540,
                          "Skipping Ethtool stats collection for interface",
                          "interface"_attr = interface);
            return nullptr;
        }

        return ethtool;
    }

    ~EthTool() {
        free(_gstrings);

        close(_fd);
    }

    // Get a list of all non-loopback interfaces for the machine
    static std::vector<std::string> interface_names() {
        struct ifaddrs* ifaddr;

        if (getifaddrs(&ifaddr) == -1) {
            auto ec = lastPosixError();
            uasserted(10985538, fmt::format("getifaddrs failed: {}", errorMessage(ec)));
        }
        ON_BLOCK_EXIT([&] { freeifaddrs(ifaddr); });

        std::set<std::string> names;
        for (ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL) {
                continue;
            }

            if ((ifa->ifa_flags & IFF_LOOPBACK) == IFF_LOOPBACK) {
                continue;
            }

            names.insert(ifa->ifa_name);
        }

        std::vector<std::string> vec;
        std::copy(names.begin(), names.end(), std::back_inserter(vec));
        return vec;
    }

    // Get a list of stats names for a given interface
    std::vector<std::string_view>& get_strings() {
        if (!_names.has_value()) {
            auto drvinfo = get_info();
            _get_strings(drvinfo.has_value() ? drvinfo->n_stats : 0);
        }

        return _names.get();
    }

    // Get a list of stats for a given interface
    std::vector<uint64_t> get_stats() {
        if (!_names.has_value()) {
            return std::vector<uint64_t>();
        }

        return _get_stats(_names->size());
    }

    // Get a some basic information about the interface
    boost::optional<ethtool_drvinfo> get_info() {
        ethtool_drvinfo drvinfo;
        memset(&drvinfo, 0, sizeof(drvinfo));
        drvinfo.cmd = ETHTOOL_GDRVINFO;

        if (_ioctlNoThrow("drvinfo", &drvinfo)) {
            return boost::none;
        }

        return boost::optional<ethtool_drvinfo>(drvinfo);
    }

    // Name of the interface this class monitors
    std::string_view name() const {
        return _interface;
    }

private:
    explicit EthTool(std::string_view interface, int fd)
        : _fd(fd), _interface(std::string(interface)) {}

    void _get_strings(size_t count) {
        _gstrings = static_cast<ethtool_gstrings*>(
            calloc(1, sizeof(ethtool_gstrings) + count * ETH_GSTRING_LEN));

        _gstrings->cmd = ETHTOOL_GSTRINGS;
        _gstrings->string_set = ETH_SS_STATS;
        _gstrings->len = count;

        _names.emplace(std::vector<std::string_view>());

        if (_ioctlNoThrow("get_strings", _gstrings)) {
            return;
        }

        char* ptr = reinterpret_cast<char*>(_gstrings) + sizeof(ethtool_gstrings);
        for (size_t i = 0; i < count; i++) {
            auto s = std::string_view(ptr);

            _names->push_back(s);

            ptr += ETH_GSTRING_LEN;
        }
    }

    std::vector<uint64_t> _get_stats(size_t count) {
        std::vector<char> stats_buf(sizeof(ethtool_stats) + count * 8,
                                    0); /* 8 is the number specfied in ethtool.h */

        ethtool_stats* stats = reinterpret_cast<ethtool_stats*>(stats_buf.data());
        stats->cmd = ETHTOOL_GSTATS;
        stats->n_stats = count;

        if (_ioctlNoThrow("get_stats", stats)) {
            return std::vector<uint64_t>();
        }

        char* ptr = reinterpret_cast<char*>(stats) + sizeof(ethtool_stats);

        std::vector<uint64_t> stats_vec(ptr, ptr + count * 8);

        return stats_vec;
    }

    // Returns non-zero on error
    int _ioctlNoThrow(std::string_view name, void* cmd) {
        ifreq ifr;

        strcpy(ifr.ifr_name, _interface.c_str());
        ifr.ifr_data = cmd;

        auto ret = ioctl(_fd, SIOCETHTOOL, &ifr);

        if (MONGO_unlikely(ret) && !_warningLogged) {
            auto ec = lastPosixError();
            _warningLogged = true;

            LOGV2_WARNING(10985553,
                          "Failed to get strings for ethtool",
                          "interface"_attr = _interface,
                          "name"_attr = name,
                          "error"_attr = errorMessage(ec));
        }

        return ret;
    }

private:
    int _fd;

    ethtool_gstrings* _gstrings{nullptr};

    boost::optional<std::vector<std::string_view>> _names;

    std::string _interface;

    bool _warningLogged{false};
};


/**
 *  Collect metrics from the Linux /proc file system.
 */
class LinuxSystemMetricsCollector final : public SystemMetricsCollector {
public:
    LinuxSystemMetricsCollector() : _disks(procparser::findPhysicalDisks("/sys/block"sv)) {
        for (const auto& disk : _disks) {
            _disksStringData.emplace_back(disk);
        }

        auto interfaces = EthTool::interface_names();

        _ethtools.reserve(interfaces.size());
        for (const auto& ifn : interfaces) {
            auto nic = EthTool::create(ifn);
            if (nic) {
                _ethtools.push_back(std::move(nic));
            }
        }
    }

    void collect(OperationContext* opCtx, BSONObjBuilder& builder) override {
        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("cpu"sv));

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
                procparser::parseProcStatFile("/proc/stat"sv, kCpuKeys, &subObjBuilder),
                &subObjBuilder);
            subObjBuilder.doneFast();
        }

        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("memory"sv));
            processStatusErrors(
                procparser::parseProcMemInfoFile("/proc/meminfo"sv, kMemKeys, &subObjBuilder),
                &subObjBuilder);
            subObjBuilder.doneFast();
        }

        {
            int thpDisabled = prctl(PR_GET_THP_DISABLE, 0, 0, 0, 0);
            if (thpDisabled >= 0) {
                BSONObjBuilder subObjBuilder(builder.subobjStart("status"));
                subObjBuilder.appendNumber("process_opting_into_THP_if_enabled", !thpDisabled);
            }
        }

        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("netstat"sv));
            processStatusErrors(procparser::parseProcNetstatFile(
                                    kNetstatKeys, "/proc/net/netstat"sv, &subObjBuilder),
                                &subObjBuilder);
            processStatusErrors(
                procparser::parseProcNetstatFile(kNetstatKeys, "/proc/net/snmp"sv, &subObjBuilder),
                &subObjBuilder);
            subObjBuilder.doneFast();
        }

        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("sockstat"sv));
            processStatusErrors(procparser::parseProcSockstatFile(
                                    kSockstatKeys, "/proc/net/sockstat"sv, &subObjBuilder),
                                &subObjBuilder);
            subObjBuilder.doneFast();
        }

        // Skip the disks section if we could not find any disks.
        // This can happen when we do not have permission to /sys/block for instance.
        if (!_disksStringData.empty()) {
            BSONObjBuilder subObjBuilder(builder.subobjStart("disks"sv));
            processStatusErrors(procparser::parseProcDiskStatsFile(
                                    "/proc/diskstats"sv, _disksStringData, &subObjBuilder),
                                &subObjBuilder);
            subObjBuilder.doneFast();
        }

        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("mounts"sv));
            processStatusErrors(
                procparser::parseProcSelfMountStatsFile("/proc/self/mountinfo"sv, &subObjBuilder),
                &subObjBuilder);
            subObjBuilder.doneFast();
        }

        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("vmstat"sv));
            processStatusErrors(
                procparser::parseProcVMStatFile("/proc/vmstat"sv, kVMKeys, &subObjBuilder),
                &subObjBuilder);
            subObjBuilder.doneFast();
        }

        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("files"sv));
            processStatusErrors(
                procparser::parseProcSysFsFileNrFile("/proc/sys/fs/file-nr"sv,
                                                     procparser::FileNrKey::kFileHandlesInUse,
                                                     &subObjBuilder),
                &subObjBuilder);
            subObjBuilder.doneFast();
        }

        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("pressure"sv));
            processStatusErrors(
                procparser::parseProcPressureFile("cpu", "/proc/pressure/cpu"sv, &subObjBuilder),
                &subObjBuilder);

            processStatusErrors(procparser::parseProcPressureFile(
                                    "memory", "/proc/pressure/memory"sv, &subObjBuilder),
                                &subObjBuilder);

            processStatusErrors(
                procparser::parseProcPressureFile("io", "/proc/pressure/io"sv, &subObjBuilder),
                &subObjBuilder);
            subObjBuilder.doneFast();
        }

        {
            BSONObjBuilder subObjBuilder(builder.subobjStart("ethtool"sv));

            for (auto& tool : _ethtools) {
                BSONObjBuilder subNICBuilder(subObjBuilder.subobjStart(tool->name()));

                auto names = tool->get_strings();
                if (names.empty()) {
                    continue;
                }

                auto stats = tool->get_stats();
                if (stats.empty()) {
                    continue;
                }
                invariant(stats.size() >= names.size());

                for (size_t i = 0; i < names.size(); i++) {
                    subNICBuilder.append(names[i], static_cast<long long>(stats[i]));
                }
            }
        }
    }

private:
    // List of physical disks to collect stats from as string from findPhysicalDisks.
    std::vector<std::string> _disks;

    // List of physical disks to collect stats from as std::string_view to pass to
    // parseProcDiskStatsFile.
    std::vector<std::string_view> _disksStringData;

    std::vector<std::unique_ptr<EthTool>> _ethtools;
};

class SimpleFunctionCollector final : public FTDCCollectorInterface {
public:
    SimpleFunctionCollector(std::string_view name,
                            unique_function<void(OperationContext*, BSONObjBuilder&)> collectFn)
        : _name(std::string{name}), _collectFn(std::move(collectFn)) {}

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


void collectUlimit(int resource, std::string_view resourceName, BSONObjBuilder& builder) {

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
    collectUlimit(RLIMIT_CPU, "cpuTime_secs"sv, builder);
    collectUlimit(RLIMIT_FSIZE, "fileSize_blocks"sv, builder);
    collectUlimit(RLIMIT_DATA, "dataSegSize_kb"sv, builder);
    collectUlimit(RLIMIT_STACK, "stackSize_kb"sv, builder);
    collectUlimit(RLIMIT_CORE, "coreFileSize_blocks"sv, builder);
    collectUlimit(RLIMIT_RSS, "residentSize_kb"sv, builder);
    collectUlimit(RLIMIT_NOFILE, "fileDescriptors"sv, builder);
    collectUlimit(RLIMIT_AS, "addressSpace_kb"sv, builder);
    collectUlimit(RLIMIT_NPROC, "processes"sv, builder);
    collectUlimit(RLIMIT_MEMLOCK, "memLock_kb"sv, builder);
    collectUlimit(RLIMIT_LOCKS, "fileLocks"sv, builder);
    collectUlimit(RLIMIT_SIGPENDING, "pendingSignals"sv, builder);
}

}  // namespace


void installSystemMetricsCollector(FTDCController* controller) {
    controller->addPeriodicCollector(std::make_unique<LinuxSystemMetricsCollector>());

    // Total max open files is only collected on rotate, since it changes infrequently
    controller->addOnRotateCollector(std::make_unique<SimpleFunctionCollector>(
        "sysMaxOpenFiles", [](OperationContext* ctx, BSONObjBuilder& builder) {
            auto status = procparser::parseProcSysFsFileNrFile(
                "/proc/sys/fs/file-nr", procparser::FileNrKey::kMaxFileHandles, &builder);
            // Handle errors here similarly to system stats.
            if (!status.isOK()) {
                builder.append("error", status.toString());
            }
        }));

    // Collect ULimits settings on rotation.
    controller->addOnRotateCollector(
        std::make_unique<SimpleFunctionCollector>("ulimits", collectUlimits));
}

}  // namespace mongo
