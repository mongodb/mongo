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


#include <boost/filesystem/exception.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "bits/types/struct_rusage.h"
#include <algorithm>
#include <climits>
#include <compare>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>  // IWYU pragma: keep
#include <functional>
#include <initializer_list>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "processinfo.h"
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"

#ifndef _WIN32
#include <sched.h>

#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/utsname.h>
#endif

#ifdef __BIONIC__
#include <android/api-level.h>
#elif __UCLIBC__
#include <features.h>
#else
#include <gnu/libc-version.h>
#endif

#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/platform/process_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/file.h"
#include "mongo/util/pcre.h"
#include "mongo/util/procparser.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/str.h"

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


#define KLONG long
#define KLF "l"

namespace mongo {

class LinuxProc {
public:
    LinuxProc(ProcessId pid) {
        auto name = fmt::format("/proc/{}/stat", pid.asUInt32());
        FILE* f = fopen(name.c_str(), "r");
        if (!f) {
            auto ec = lastSystemError();
            msgasserted(13538, fmt::format("couldn't open [{}] {}", name, errorMessage(ec)));
        }
        int found = fscanf(f,
                           "%d %127s %c "
                           "%d %d %d %d %d "
                           "%lu %lu %lu %lu %lu "
                           "%lu %lu %ld %ld " /* utime stime cutime cstime */
                           "%ld %ld "
                           "%ld "
                           "%ld "
                           "%lu " /* start_time */
                           "%lu "
                           "%ld "  // rss
                           "%lu %" KLF "u %" KLF "u %" KLF "u %" KLF "u %" KLF "u "
                           /*
                             "%*s %*s %*s %*s "
                             "%"KLF"u %*lu %*lu "
                             "%d %d "
                             "%lu %lu"
                           */

                           ,

                           &_pid,
                           _comm,
                           &_state,
                           &_ppid,
                           &_pgrp,
                           &_session,
                           &_tty,
                           &_tpgid,
                           &_flags,
                           &_min_flt,
                           &_cmin_flt,
                           &_maj_flt,
                           &_cmaj_flt,
                           &_utime,
                           &_stime,
                           &_cutime,
                           &_cstime,
                           &_priority,
                           &_nice,
                           &_nlwp,
                           &_alarm,
                           &_start_time,
                           &_vsize,
                           &_rss,
                           &_rss_rlim,
                           &_start_code,
                           &_end_code,
                           &_start_stack,
                           &_kstk_esp,
                           &_kstk_eip

                           /*
                             &_wchan,
                             &_exit_signal, &_processor,
                             &_rtprio, &_sched
                           */
        );
        massert(13539, fmt::format("couldn't parse [{}]", name).c_str(), found != 0);
        fclose(f);
    }

    unsigned long getVirtualMemorySize() {
        return _vsize;
    }

    unsigned long getResidentSizeInPages() {
        return (unsigned long)_rss;
    }

    int _pid;
    // The process ID.

    char _comm[128];
    // The filename of the executable, in parentheses.  This is visible whether or not the
    // executable is swapped out.

    char _state;
    // One character from the string "RSDZTW" where R is running, S is sleeping in an interruptible
    // wait, D is waiting in uninterruptible disk sleep, Z is zombie, T is traced or stopped (on a
    // signal), and W is paging.

    int _ppid;
    // The PID of the parent.

    int _pgrp;
    // The process group ID of the process.

    int _session;
    // The session ID of the process.

    int _tty;
    // The tty the process uses.

    int _tpgid;
    // The process group ID of the process which currently owns the tty that the process is
    // connected to.

    unsigned long _flags;  // %lu
    // The  kernel flags word of the process. For bit meanings, see the PF_* defines in
    // <linux/sched.h>.  Details depend on the kernel version.

    unsigned long _min_flt;  // %lu
    // The number of minor faults the process has made which have not required loading a memory page
    // from disk.

    unsigned long _cmin_flt;  // %lu
    // The number of minor faults that the process

    unsigned long _maj_flt;  // %lu
    // The number of major faults the process has made which have required loading a memory page
    // from disk.

    unsigned long _cmaj_flt;  // %lu
    // The number of major faults that the process

    unsigned long _utime;  // %lu
    // The number of jiffies that this process has been scheduled in user mode.

    unsigned long _stime;  //  %lu
    // The number of jiffies that this process has been scheduled in kernel mode.

    long _cutime;  // %ld
    // The number of jiffies that this removed field.

    long _cstime;  // %ld

    long _priority;
    long _nice;

    long _nlwp;  // %ld
    // number of threads

    unsigned long _alarm;
    // The time in jiffies before the next SIGALRM is sent to the process due to an interval timer.
    // (unused since 2.6.17)

    unsigned long _start_time;  // %lu
    // The time in jiffies the process started after system boot.

    unsigned long _vsize;  // %lu
    // Virtual memory size in bytes.

    long _rss;  // %ld
    // Resident Set Size: number of pages the process has in real memory, minus 3 for administrative
    // purposes. This is just the pages which count  towards  text,  data, or stack space.  This
    // does not include pages which have not been demand-loaded in, or which are swapped out

    unsigned long _rss_rlim;  // %lu
    // Current limit in bytes on the rss of the process (usually 4294967295 on i386).

    unsigned long _start_code;  // %lu
    // The address above which program text can run.

    unsigned long _end_code;  // %lu
    // The address below which program text can run.

    unsigned long _start_stack;  // %lu
    // The address of the start of the stack.

    unsigned long _kstk_esp;  // %lu
    // The current value of esp (stack pointer), as found in the kernel stack page for the process.

    unsigned long _kstk_eip;  // %lu
    // The current EIP (instruction pointer).
};

namespace {

// As described in the /proc/[pid]/mountinfo section of `man 5 proc`:
//
// 36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 - ext3 /dev/root rw,errors=continue
// |  |  |    |     |     |          |          |      |     |
// (1)(2)(3:4)(5)   (6)   (7)        (8)        (9)   (10)   (11)
struct MountRecord {
    bool parseLine(const std::string& line) {
        static const pcre::Regex kRe{
            //    (1)   (2)   (3)   (4)   (5)   (6)   (7)   (8)                (9)   (10)  (11)
            R"re(^(\d+) (\d+) (\d+):(\d+) (\S+) (\S+) (\S+) ((?:\S+:\S+ ?)*) - (\S+) (\S+) (\S+)$)re"};
        auto m = kRe.matchView(line);
        if (!m)
            return false;
        size_t i = 1;
        auto load = [&](auto& var) {
            using T = std::decay_t<decltype(var)>;
            std::string nextString{m[i++]};
            if constexpr (std::is_same_v<T, int>) {
                var = std::stoi(nextString);
            } else {
                var = std::move(nextString);
            }
        };
        load(mountId);
        load(parentId);
        load(major);
        load(minor);
        load(root);
        load(mountPoint);
        load(options);
        load(fields);
        load(type);
        load(source);
        load(superOpt);
        return true;
    }

    void appendBSON(BSONObjBuilder& bob) const {
        bob.append("mountId", mountId)
            .append("parentId", parentId)
            .append("major", major)
            .append("minor", minor)
            .append("root", root)
            .append("mountPoint", mountPoint)
            .append("options", options)
            .append("fields", fields)
            .append("type", type)
            .append("source", source)
            .append("superOpt", superOpt);
    }

    int mountId;             //  (1) unique ID for the mount
    int parentId;            //  (2) the ID of the parent mount (self for the root mount)
    int major;               //  (3) major block device number (see stat(2))
    int minor;               //  (4) minor block device number
    std::string root;        //  (5) path in filesystem forming the root
    std::string mountPoint;  //  (6) the mount point relative to the process's root
    std::string options;     //  (7) per-mount options (see mount(2)).
    std::string fields;      //  (8) zero or more: "tag[:value]" fields
    std::string type;        //  (9) filesystem type: "type[.subtype]"
    std::string source;      //  (10) fs-specific information or "none"
    std::string superOpt;    //  (11) per-superblock options (see mount(2))
};

struct DiskStat {
    int major;
    int minor;
    // See https://www.kernel.org/doc/Documentation/ABI/testing/procfs-diskstats
    const std::vector<std::string> statsLabels = {"deviceName",
                                                  "reads",
                                                  "readsMerged",
                                                  "readsSectors",
                                                  "readsMs",
                                                  "writes",
                                                  "writesMerged",
                                                  "writesSectors",
                                                  "writesMs",
                                                  "ioInProgress",
                                                  "ioMs",
                                                  "ioMsWeighted",
                                                  "discards",
                                                  "discardsMerged",
                                                  "discardsSectors",
                                                  "discardsMs",
                                                  "flushes",
                                                  "flushesMs"};
    std::vector<std::string> statsValues;

    void appendBSON(BSONObjBuilder& bob) const {
        int iterations = std::min(statsLabels.size(), statsValues.size());
        for (int i = 0; i < iterations; ++i) {
            bob.append(statsLabels[i], statsValues[i]);
        }
    }

    bool readLine(const std::string& line) {
        std::istringstream iss(line);

        iss >> major;
        iss >> minor;
        if (iss.fail()) {
            LOGV2(5963301, "Malformed diskstats line", "line"_attr = line);
            return false;
        }

        std::string val;
        while (iss >> val) {
            statsValues.push_back(val);
        }
        return true;
    }
};

void appendMountInfo(BSONObjBuilder& bob) {
    std::set<std::pair<int, int>> majorMinors;
    std::map<std::pair<int, int>, DiskStat> diskStats;
    std::ifstream ifs;

    std::string line;
    ifs.open("/proc/diskstats");
    if (ifs) {
        while (ifs && getline(ifs, line)) {
            DiskStat ds;
            if (ds.readLine(line)) {
                std::pair<int, int> majorMinor = std::make_pair(ds.major, ds.minor);
                majorMinors.insert(majorMinor);
                diskStats.insert({majorMinor, ds});
            }
        }
    }
    ifs.close();

    std::map<std::pair<int, int>, MountRecord> mountRecords;
    ifs.open("/proc/self/mountinfo");
    if (ifs) {
        while (ifs && getline(ifs, line)) {
            MountRecord mr;
            mr.parseLine(line);
            std::pair<int, int> majorMinor = std::make_pair(mr.major, mr.minor);
            majorMinors.insert(majorMinor);
            mountRecords.insert({majorMinor, mr});
        }
    }
    ifs.close();

    BSONArrayBuilder arr = bob.subarrayStart("mountInfo");
    for (const auto& majorMinor : majorMinors) {
        auto arrBob = BSONObjBuilder(arr.subobjStart());

        auto mr = mountRecords.find(majorMinor);
        if (mr != mountRecords.end()) {
            mr->second.appendBSON(arrBob);
        }

        auto ds = diskStats.find(majorMinor);
        if (ds != diskStats.end()) {
            ds->second.appendBSON(arrBob);
        }
    }
}

class CpuInfoParser {
public:
    struct LineProcessor {
        LineProcessor(std::string pattern, std::function<void(const std::string&)> f)
            : regex{std::make_shared<pcre::Regex>(std::move(pattern))}, f{std::move(f)} {}
        std::shared_ptr<pcre::Regex> regex;
        std::function<void(const std::string&)> f;
    };
    std::vector<LineProcessor> lineProcessors;
    std::function<void()> recordProcessor;
    void run() {
        std::ifstream f("/proc/cpuinfo");
        if (!f)
            return;

        bool readSuccess;
        bool unprocessed = false;
        static StaticImmortal<pcre::Regex> lineRegex(R"re(^(.*?)\s*:\s*(.*)$)re");
        do {
            std::string fstr;
            readSuccess = f && std::getline(f, fstr);
            if (readSuccess && !fstr.empty()) {
                auto m = lineRegex->matchView(fstr);
                if (!m)
                    continue;
                std::string key{m[1]};
                std::string value{m[2]};
                for (auto&& [lpr, lpf] : lineProcessors) {
                    if (lpr->matchView(key, pcre::ANCHORED | pcre::ENDANCHORED))
                        lpf(value);
                }
                unprocessed = true;
            } else if (unprocessed) {
                recordProcessor();
                unprocessed = false;
            }
        } while (readSuccess);
    }
};

class LinuxSysHelper {
public:
    /**
     * Read the first 1023 bytes from a file
     */
    static std::string parseLineFromFile(const char* fname) {
        FILE* f;
        char fstr[1024] = {0};

        f = fopen(fname, "r");
        if (f != nullptr) {
            if (fgets(fstr, 1023, f) != nullptr)
                fstr[strlen(fstr) < 1 ? 0 : strlen(fstr) - 1] = '\0';
            fclose(f);
        }
        return fstr;
    }


    /**
     * count the number of physical cores
     */
    static void getNumPhysicalCores(int& physicalCores) {

        /* In /proc/cpuinfo core ids are only unique within a particular physical unit, AKA a cpu
         * package, so to count the total cores we need to count the unique pairs of core id and
         * physical id*/
        struct CpuId {
            std::string core;
            std::string physical;
        };

        CpuId parsedCpuId;

        auto cmp = [](auto&& a, auto&& b) {
            auto tupLens = [](auto&& o) {
                return std::tie(o.core, o.physical);
            };
            return tupLens(a) < tupLens(b);
        };
        std::set<CpuId, decltype(cmp)> cpuIds(cmp);

        CpuInfoParser cpuInfoParser{{
                                        {"physical id",
                                         [&](const std::string& value) {
                                             parsedCpuId.physical = value;
                                         }},
                                        {"core id",
                                         [&](const std::string& value) {
                                             parsedCpuId.core = value;
                                         }},
                                    },
                                    [&]() {
                                        cpuIds.insert(parsedCpuId);
                                        parsedCpuId = CpuId{};
                                    }};
        cpuInfoParser.run();

        physicalCores = cpuIds.size();
    }

    /**
     * count the number of processor packages
     */
    static int getNumCpuSockets() {
        std::set<std::string> socketIds;

        CpuInfoParser cpuInfoParser{{
                                        {"physical id",
                                         [&](const std::string& value) {
                                             socketIds.insert(value);
                                         }},
                                    },
                                    []() {
                                    }};
        cpuInfoParser.run();

        // On ARM64, the "physical id" field is unpopulated, causing there to be 0 sockets found. In
        // this case, we default to 1.
        return std::max(socketIds.size(), 1ul);
    }

    /**
     * Get some details about the CPU
     */
    static void getCpuInfo(int& procCount,
                           std::string& modelString,
                           std::string& freq,
                           std::string& features,
                           std::string& cpuImplementer,
                           std::string& cpuArchitecture,
                           std::string& cpuVariant,
                           std::string& cpuPart,
                           std::string& cpuRevision) {

        procCount = 0;

        CpuInfoParser cpuInfoParser{{
#ifdef __s390x__
                                        {R"re(processor\s+\d+)re",
                                         [&](const std::string& value) {
                                             procCount++;
                                         }},
                                        {"cpu MHz static",
                                         [&](const std::string& value) {
                                             freq = value;
                                         }},
                                        {"features",
                                         [&](const std::string& value) {
                                             features = value;
                                         }},
#else
                                        {"processor",
                                         [&](const std::string& value) {
                                             procCount++;
                                         }},
                                        {"model name",
                                         [&](const std::string& value) {
                                             modelString = value;
                                         }},
                                        {"cpu MHz",
                                         [&](const std::string& value) {
                                             freq = value;
                                         }},
                                        {"flags",
                                         [&](const std::string& value) {
                                             features = value;
                                         }},
                                        {"CPU implementer",
                                         [&](const std::string& value) {
                                             cpuImplementer = value;
                                         }},
                                        {"CPU architecture",
                                         [&](const std::string& value) {
                                             cpuArchitecture = value;
                                         }},
                                        {"CPU variant",
                                         [&](const std::string& value) {
                                             cpuVariant = value;
                                         }},
                                        {"CPU part",
                                         [&](const std::string& value) {
                                             cpuPart = value;
                                         }},
                                        {"CPU revision",
                                         [&](const std::string& value) {
                                             cpuRevision = value;
                                         }},
#endif
                                    },
                                    []() {
                                    }};
        cpuInfoParser.run();
    }

    /**
     * Determine linux distro and version
     */
    static void getLinuxDistro(std::string& name, std::string& version) {
        char buf[4096] = {0};

        // try lsb file first
        if (boost::filesystem::exists("/etc/lsb-release")) {
            File f;
            f.open("/etc/lsb-release", true);
            if (!f.is_open() || f.bad())
                return;
            f.read(0, buf, f.len() > 4095 ? 4095 : f.len());

            // find the distribution name and version in the contents.
            // format:  KEY=VAL\n
            std::string contents = buf;
            unsigned lineCnt = 0;
            try {
                while (lineCnt < contents.length() - 1 &&
                       contents.substr(lineCnt).find('\n') != std::string::npos) {
                    // until we hit the last newline or eof
                    std::string line =
                        contents.substr(lineCnt, contents.substr(lineCnt).find('\n'));
                    lineCnt += contents.substr(lineCnt).find('\n') + 1;
                    size_t delim = line.find('=');
                    std::string key = line.substr(0, delim);
                    std::string val = line.substr(delim + 1);  // 0-based offset of delim
                    if (key.compare("DISTRIB_ID") == 0)
                        name = val;
                    if (std::string(key).compare("DISTRIB_RELEASE") == 0)
                        version = val;
                }
            } catch (const std::out_of_range&) {
                // attempted to get invalid substr
            }
            // return with lsb-release data if we found both the name and version
            if (!name.empty() && !version.empty()) {
                return;
            }
        }

        // try known flat-text file locations
        // format: Slackware-x86_64 13.0, Red Hat Enterprise Linux Server release 5.6 (Tikanga),
        // etc.
        typedef std::vector<std::string> pathvec;
        pathvec paths;
        pathvec::const_iterator i;
        bool found = false;
        paths.push_back("/etc/system-release");
        paths.push_back("/etc/redhat-release");
        paths.push_back("/etc/gentoo-release");
        paths.push_back("/etc/novell-release");
        paths.push_back("/etc/gentoo-release");
        paths.push_back("/etc/SuSE-release");
        paths.push_back("/etc/SUSE-release");
        paths.push_back("/etc/sles-release");
        paths.push_back("/etc/debian_release");
        paths.push_back("/etc/slackware-version");
        paths.push_back("/etc/centos-release");
        paths.push_back("/etc/os-release");

        for (i = paths.begin(); i != paths.end(); ++i) {
            // for each path
            if (boost::filesystem::exists(*i)) {
                // if the file exists, break
                found = true;
                break;
            }
        }

        if (found) {
            // found a file
            File f;
            f.open(i->c_str(), true);
            if (!f.is_open() || f.bad())
                // file exists but can't be opened
                return;

            // read up to 512 bytes
            int len = f.len() > 512 ? 512 : f.len();
            f.read(0, buf, len);
            buf[len] = '\0';
            name = buf;
            size_t nl = 0;
            if ((nl = name.find('\n', nl)) != std::string::npos)
                // stop at first newline
                name.erase(nl);
        } else {
            name = "unknown";
        }

        // There is no standard format for name and version so use the kernel version.
        version = "Kernel ";
        version += LinuxSysHelper::parseLineFromFile("/proc/sys/kernel/osrelease");
    }

    /**
     * Get system memory total
     */
    static unsigned long long getSystemMemorySize() {
        std::string meminfo = parseLineFromFile("/proc/meminfo");
        size_t lineOff = 0;
        if (!meminfo.empty() && (lineOff = meminfo.find("MemTotal")) != std::string::npos) {
            // found MemTotal line.  capture everything between 'MemTotal:' and ' kB'.
            lineOff = meminfo.substr(lineOff).find(':') + 1;
            meminfo = meminfo.substr(lineOff, meminfo.substr(lineOff).find("kB") - 1);
            lineOff = 0;

            // trim whitespace and append 000 to replace kB.
            while (ctype::isSpace(meminfo.at(lineOff)))
                lineOff++;
            meminfo = meminfo.substr(lineOff);

            unsigned long long systemMem = 0;
            if (mongo::NumberParser{}(meminfo, &systemMem).isOK()) {
                return systemMem * 1024;  // convert from kB to bytes
            } else
                LOGV2(23338, "Unable to collect system memory information");
        }
        return 0;
    }

    /**
     * Get memory limit for the process.
     * If memory is being limited by the applied control group and it's less
     * than the OS system memory (default cgroup limit is ulonglong max) let's
     * return the actual memory we'll have available to the process.
     */
    static unsigned long long getMemorySizeLimit() {
        const unsigned long long systemMemBytes = getSystemMemorySize();
        for (const char* file : {
                 "/sys/fs/cgroup/memory.max",                   // cgroups v2
                 "/sys/fs/cgroup/memory/memory.limit_in_bytes"  // cgroups v1
             }) {
            unsigned long long groupMemBytes = 0;
            std::string groupLimit = parseLineFromFile(file);
            if (!groupLimit.empty() && NumberParser{}(groupLimit, &groupMemBytes).isOK()) {
                return std::min(systemMemBytes, groupMemBytes);
            }
        }
        return systemMemBytes;
    }

    /**
     * Get Cgroup path of the process.
     * return string path of cgrouop. If no cgroup v2, return an empty string.
     */
    static std::string getCgroupV2Path(const ProcessId& pid) {
        const auto line = LinuxSysHelper::parseLineFromFile(
            fmt::format("/proc/{}/cgroup", pid.asUInt32()).c_str());
        if (line.empty()) {
            return {};
        }
        // The entry for cgroup v2 is always in the format “0::$PATH”.
        const StringData prefixV2 = "0::"_sd;
        const size_t prefixLength = prefixV2.length();

        // Check if the input starts with the prefix
        if (StringData{line}.starts_with(prefixV2)) {
            // cgroup v2.
            return fmt::format("/sys/fs/cgroup{}", line.substr(prefixLength));
        } else {
            // cgroup v1.
            return {};
        }
    }

    static void getCpuCgroupV2Info(const ProcessId& pid,
                                   std::string& cpuMax,
                                   std::string& cpuMaxBurst,
                                   std::string& cpuUclampMin,
                                   std::string& cpuUclampMax,
                                   std::string& cpuWeight) {
        auto path = getCgroupV2Path(pid);
        if (path.empty()) {
            return;
        }
        auto parseLineOrDefault = [](const std::string& fileName) {
            auto lineStr = parseLineFromFile(fileName.c_str());
            return lineStr.empty() ? "default" : lineStr;
        };
        cpuMax = parseLineOrDefault(fmt::format("{}/cpu.max", path));
        cpuMaxBurst = parseLineOrDefault(fmt::format("{}/cpu.max.burst", path));
        cpuUclampMin = parseLineOrDefault(fmt::format("{}/cpu.uclamp.min", path));
        cpuUclampMax = parseLineOrDefault(fmt::format("{}/cpu.uclamp.max", path));
        cpuWeight = parseLineOrDefault(fmt::format("{}/cpu.weight", path));
    }

    static int parseProcIntWithDefault(const char* path, int defaultValue) {
        std::ifstream proc{path};
        if (!proc) {
            LOGV2(10181001,
                  "Unable to open procfs path, falling back to default value",
                  "path"_attr = path,
                  "default"_attr = defaultValue);
            return defaultValue;
        }
        int value;
        if (!(proc >> value)) {
            LOGV2(10181002,
                  "Unable to read an integer from procfs file, falling back to default value",
                  "path"_attr = path,
                  "default"_attr = defaultValue);
            return defaultValue;
        }
        std::string remainingNonWhitespace;
        if (proc >> remainingNonWhitespace) {
            LOGV2(10181003,
                  "procfs file contains trailing non-integer data, falling back to default "
                  "value",
                  "path"_attr = path,
                  "int_data"_attr = value,
                  "extra_data"_attr = remainingNonWhitespace,
                  "default"_attr = defaultValue);
            return defaultValue;
        }
        return value;
    }
};

void appendIfExists(BSONObjBuilder* bob, StringData key, StringData value) {
    if (!value.empty()) {
        bob->append(key, value);
    }
}

void collectPressureStallInfo(BSONObjBuilder& builder) {

    auto parsePressureFile = [](StringData key, StringData filename, BSONObjBuilder& bob) {
        BSONObjBuilder psiParseBuilder;
        auto status = procparser::parseProcPressureFile(key, filename, &psiParseBuilder);
        if (status.isOK()) {
            bob.appendElements(psiParseBuilder.obj());
        }
        return status.isOK();
    };

    BSONObjBuilder psiBuilder;
    bool parseStatus = false;

    parseStatus |= parsePressureFile("memory", "/proc/pressure/memory"_sd, psiBuilder);
    parseStatus |= parsePressureFile("cpu", "/proc/pressure/cpu"_sd, psiBuilder);
    parseStatus |= parsePressureFile("io", "/proc/pressure/io"_sd, psiBuilder);

    if (parseStatus) {
        builder.append("pressure"_sd, psiBuilder.obj());
    }
}

/**
 * If the process is running with (cc)NUMA enabled, return the number of NUMA nodes. Else, return 0.
 */
unsigned long countNumaNodes() {
    bool hasMultipleNodes = false;
    bool hasNumaMaps = false;

    try {
        hasMultipleNodes = boost::filesystem::exists("/sys/devices/system/node/node1");
        hasNumaMaps = boost::filesystem::exists("/proc/self/numa_maps");

        if (hasMultipleNodes && hasNumaMaps) {
            // proc is populated with numa entries

            // read the second column of first line to determine numa state
            // ('default' = enabled, 'interleave' = disabled).  Logic from version.cpp's warnings.
            std::string line =
                LinuxSysHelper::parseLineFromFile("/proc/self/numa_maps").append(" \0");
            size_t pos = line.find(' ');
            if (pos != std::string::npos &&
                line.substr(pos + 1, 10).find("interleave") == std::string::npos) {
                // interleave not found, count NUMA nodes by finding the highest numbered node file
                unsigned long i = 2;
                while (boost::filesystem::exists(
                    std::string(str::stream() << "/sys/devices/system/node/node" << i++)))
                    ;
                return i - 1;
            }
        }
    } catch (boost::filesystem::filesystem_error& e) {
        LOGV2(23340,
              "WARNING: Cannot detect if NUMA interleaving is enabled. Failed to probe",
              "path"_attr = e.path1().string(),
              "reason"_attr = e.code().message());
    }
    return 0;
}

/**
 * Append CPU Cgroup v2 info of the current process.
 */
void appendCpuCgrouopV2Info(BSONObjBuilder& bob) {
    std::string cpuMax, cpuMaxBurst, cpuUclampMin, cpuUclampMax, cpuWeight;
    LinuxSysHelper::getCpuCgroupV2Info(
        ProcessId::getCurrent(), cpuMax, cpuMaxBurst, cpuUclampMin, cpuUclampMax, cpuWeight);
    appendIfExists(&bob, "cpuMax"_sd, cpuMax);
    appendIfExists(&bob, "cpuMaxBurst"_sd, cpuMaxBurst);
    appendIfExists(&bob, "cpuUclampMin"_sd, cpuUclampMin);
    appendIfExists(&bob, "cpuUclampMax"_sd, cpuUclampMax);
    appendIfExists(&bob, "cpuWeight"_sd, cpuWeight);
}

}  // namespace

ProcessInfo::ProcessInfo(ProcessId pid) : _pid(pid) {}

ProcessInfo::~ProcessInfo() {}

bool ProcessInfo::supported() {
    return true;
}

// get the number of CPUs available to the current process
boost::optional<unsigned long> ProcessInfo::getNumCoresForProcess() {
    cpu_set_t set;

    if (sched_getaffinity(0, sizeof(cpu_set_t), &set) == 0) {
#ifdef CPU_COUNT  // glibc >= 2.6 has CPU_COUNT defined
        return CPU_COUNT(&set);
#else
        unsigned long count = 0;
        for (size_t i = 0; i < CPU_SETSIZE; i++)
            if (CPU_ISSET(i, &set))
                count++;
        if (count > 0)
            return count;
#endif
    }

    auto ec = lastSystemError();
    LOGV2(8366600,
          "sched_getaffinity failed to collect cpu_set info",
          "error"_attr = errorMessage(ec));

    return boost::none;
}

int ProcessInfo::getVirtualMemorySize() {
    LinuxProc p(_pid);
    return (int)(p.getVirtualMemorySize() / (1024.0 * 1024));
}

int ProcessInfo::getResidentSize() {
    LinuxProc p(_pid);
    return (int)((p.getResidentSizeInPages() * getPageSize()) / (1024.0 * 1024));
}

StatusWith<std::string> ProcessInfo::readTransparentHugePagesParameter(StringData parameter,
                                                                       StringData directory) {
    auto line =
        LinuxSysHelper::parseLineFromFile(fmt::format("{}/{}", directory, parameter).c_str());
    if (line.empty()) {
        return {ErrorCodes::NonExistentPath,
                fmt::format("Empty or non-existent file at {}/{}", directory, parameter)};
    }

    std::string opMode;
    std::string::size_type posBegin = line.find('[');
    std::string::size_type posEnd = line.find(']');
    if (posBegin == std::string::npos || posEnd == std::string::npos || posBegin >= posEnd) {
        return {ErrorCodes::FailedToParse, fmt::format("Cannot parse line: '{}'", line)};
    }

    opMode = line.substr(posBegin + 1, posEnd - posBegin - 1);
    if (opMode.empty()) {
        return {ErrorCodes::BadValue,
                fmt::format("Invalid mode in {}/{}: '{}'", directory, parameter, line)};
    }

    // Check against acceptable values of opMode.
    static constexpr std::array acceptableValues{
        "always"_sd,
        "defer"_sd,
        "defer+madvise"_sd,
        "madvise"_sd,
        "never"_sd,
    };
    if (std::find(acceptableValues.begin(), acceptableValues.end(), opMode) ==
        acceptableValues.end()) {
        return {
            ErrorCodes::BadValue,
            fmt::format(
                "** WARNING: unrecognized transparent Huge Pages mode of operation in {}/{}: '{}'",
                directory,
                parameter,
                opMode)};
    }

    return std::move(opMode);
}

bool ProcessInfo::checkGlibcRseqTunable() {
    StringData glibcEnv = getenv(kGlibcTunableEnvVar);
    auto foundIndex = glibcEnv.find(kRseqKey);

    if (foundIndex != std::string::npos) {
        try {
            auto rseqSetting = glibcEnv.at(foundIndex + strlen(kRseqKey) + 1);

            return std::stoi(std::string{rseqSetting}) == 0;
        } catch (std::exception const&) {
            return false;
        }
    }

    return false;
}

void ProcessInfo::getExtraInfo(BSONObjBuilder& info) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);

    /*
     * The actual rusage struct only works in terms of longs and time_ts.
     * Since both are system dependent, I am converting to int64_t and taking a small hit from the
     * FP processor and the BSONBuilder compression. At worst, this calls 100x/sec.
     */
    auto appendTime = [&info](StringData fieldName, struct timeval tv) {
        auto value = (static_cast<int64_t>(tv.tv_sec) * 1000 * 1000) + tv.tv_usec;
        info.append(fieldName, value);
    };

    auto appendNumber = [&info](StringData fieldName, auto value) {
        info.append(fieldName, static_cast<int64_t>(value));
    };

    appendTime("user_time_us", ru.ru_utime);
    appendTime("system_time_us", ru.ru_stime);

    // ru_maxrss is duplicated in getResidentSizeInPages
    // (/proc may or may not use getrusage(2) as well)
    appendNumber("maximum_resident_set_kb", ru.ru_maxrss);

    appendNumber("input_blocks", ru.ru_inblock);
    appendNumber("output_blocks", ru.ru_oublock);

    appendNumber("page_reclaims", ru.ru_minflt);
    appendNumber("page_faults", ru.ru_majflt);

    appendNumber("voluntary_context_switches", ru.ru_nvcsw);
    appendNumber("involuntary_context_switches", ru.ru_nivcsw);

    LinuxProc p(_pid);

    // Append the number of thread in use
    appendNumber("threads", p._nlwp);

    // Append Pressure Stall Information (PSI)
    collectPressureStallInfo(info);
}

/**
 * Save a BSON obj representing the host system's details
 */
void ProcessInfo::SystemInfo::collectSystemInfo() {
    utsname unameData;
    std::string distroName, distroVersion;
    std::string cpuString, cpuFreq, cpuFeatures;
    std::string cpuImplementer, cpuArchitecture, cpuVariant, cpuPart, cpuRevision;
    int cpuCount;
    int physicalCores;
    int cpuSockets;

    std::string verSig = LinuxSysHelper::parseLineFromFile("/proc/version_signature");
    LinuxSysHelper::getCpuInfo(cpuCount,
                               cpuString,
                               cpuFreq,
                               cpuFeatures,
                               cpuImplementer,
                               cpuArchitecture,
                               cpuVariant,
                               cpuPart,
                               cpuRevision);
    LinuxSysHelper::getNumPhysicalCores(physicalCores);
    cpuSockets = LinuxSysHelper::getNumCpuSockets();
    LinuxSysHelper::getLinuxDistro(distroName, distroVersion);

    if (uname(&unameData) == -1) {
        auto ec = lastSystemError();
        LOGV2(23339,
              "Unable to collect detailed system information",
              "error"_attr = errorMessage(ec));
    }

    osType = "Linux";
    osName = distroName;
    osVersion = distroVersion;
    memSize = LinuxSysHelper::getSystemMemorySize();
    memLimit = LinuxSysHelper::getMemorySizeLimit();
    addrSize = sizeof(void*) * CHAR_BIT;
    numCores = cpuCount;
    numPhysicalCores = physicalCores;
    numCpuSockets = cpuSockets;
    pageSize = static_cast<unsigned long long>(sysconf(_SC_PAGESIZE));
    cpuArch = unameData.machine;
    numNumaNodes = countNumaNodes();
    hasNuma = numNumaNodes;
    defaultListenBacklog =
        LinuxSysHelper::parseProcIntWithDefault("/proc/sys/net/core/somaxconn", SOMAXCONN);

    BSONObjBuilder bExtra;
    bExtra.append("versionString", LinuxSysHelper::parseLineFromFile("/proc/version"));
#ifdef __BIONIC__
    std::stringstream ss;
    ss << "bionic (android api " << __ANDROID_API__ << ")";
    bExtra.append("libcVersion", ss.str());
#elif __UCLIBC__
    std::stringstream ss;
    ss << "uClibc-" << __UCLIBC_MAJOR__ << "." << __UCLIBC_MINOR__ << "." << __UCLIBC_SUBLEVEL__;
    bExtra.append("libcVersion", ss.str());
#else
    bExtra.append("libcVersion", gnu_get_libc_version());
#endif
    if (!verSig.empty())
        // optional
        bExtra.append("versionSignature", verSig);

    bExtra.append("kernelVersion", unameData.release);
    bExtra.append("cpuString", cpuString);
    bExtra.append("cpuFrequencyMHz", cpuFreq);
    bExtra.append("cpuFeatures", cpuFeatures);
    bExtra.append("pageSize", static_cast<long long>(pageSize));
    bExtra.append("numPages", static_cast<int>(sysconf(_SC_PHYS_PAGES)));
    bExtra.append("maxOpenFiles", static_cast<int>(sysconf(_SC_OPEN_MAX)));

    // Append ARM-specific fields, if they exist.
    // Can be mapped to model name by referencing sys-utils/lscpu-arm.c
    appendIfExists(&bExtra, "cpuImplementer", cpuImplementer);
    appendIfExists(&bExtra, "cpuArchitecture", cpuArchitecture);
    appendIfExists(&bExtra, "cpuVariant", cpuVariant);
    appendIfExists(&bExtra, "cpuPart", cpuPart);
    appendIfExists(&bExtra, "cpuRevision", cpuRevision);

    // Append CPU Cgroup v2 information, if they exist.
    appendCpuCgrouopV2Info(bExtra);

    if (auto res = ProcessInfo::readTransparentHugePagesParameter("enabled"); res.isOK()) {
        appendIfExists(&bExtra, "thp_enabled", res.getValue());
    }

    if (auto res = ProcessInfo::readTransparentHugePagesParameter("defrag"); res.isOK()) {
        appendIfExists(&bExtra, "thp_defrag", res.getValue());
    }

    appendIfExists(
        &bExtra,
        "thp_max_ptes_none",
        LinuxSysHelper::parseLineFromFile(
            fmt::format("{}/khugepaged/max_ptes_none", kTranparentHugepageDirectory).c_str()));
    appendIfExists(&bExtra,
                   "overcommit_memory",
                   LinuxSysHelper::parseLineFromFile("/proc/sys/vm/overcommit_memory"));

#if defined(MONGO_CONFIG_GLIBC_RSEQ)
    bExtra.append("glibc_rseq_present", true);
    bExtra.append("glibc_pthread_rseq_disabled", checkGlibcRseqTunable());
#else
    bExtra.append("glibc_rseq_present", false);
#endif

    appendMountInfo(bExtra);

    _extraStats = bExtra.obj();
}

}  // namespace mongo
