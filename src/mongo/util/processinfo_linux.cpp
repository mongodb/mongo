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


#include "mongo/platform/basic.h"

#include "processinfo.h"

#include <fstream>
#include <iostream>
#include <malloc.h>
#include <sched.h>
#include <stdio.h>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifdef __BIONIC__
#include <android/api-level.h>
#elif __UCLIBC__
#include <features.h>
#else
#include <gnu/libc-version.h>
#endif

#include <boost/filesystem.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <fmt/format.h>

#include "mongo/base/parse_number.h"
#include "mongo/logv2/log.h"
#include "mongo/util/ctype.h"
#include "mongo/util/file.h"
#include "mongo/util/pcre.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


#define KLONG long
#define KLF "l"

namespace mongo {

using namespace fmt::literals;

class LinuxProc {
public:
    LinuxProc(ProcessId pid) {
        auto name = "/proc/{}/stat"_format(pid.asUInt32());
        FILE* f = fopen(name.c_str(), "r");
        if (!f) {
            auto ec = lastSystemError();
            msgasserted(13538, "couldn't open [{}] {}"_format(name, errorMessage(ec)));
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
        massert(13539, "couldn't parse [{}]"_format(name).c_str(), found != 0);
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

void appendMountInfo(BSONObjBuilder& bob) {
    std::ifstream ifs("/proc/self/mountinfo");
    if (!ifs)
        return;
    BSONArrayBuilder arr = bob.subarrayStart("mountInfo");
    std::string line;
    MountRecord rec;
    while (ifs && getline(ifs, line)) {
        if (rec.parseLine(line)) {
            auto bob = BSONObjBuilder(arr.subobjStart());
            rec.appendBSON(bob);
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

}  // namespace

class LinuxSysHelper {
public:
    /**
     * Read the first 1023 bytes from a file
     */
    static std::string readLineFromFile(const char* fname) {
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
            auto tupLens = [](auto&& o) { return std::tie(o.core, o.physical); };
            return tupLens(a) < tupLens(b);
        };
        std::set<CpuId, decltype(cmp)> cpuIds(cmp);

        CpuInfoParser cpuInfoParser{
            {
                {"physical id", [&](const std::string& value) { parsedCpuId.physical = value; }},
                {"core id", [&](const std::string& value) { parsedCpuId.core = value; }},
            },
            [&]() {
                cpuIds.insert(parsedCpuId);
                parsedCpuId = CpuId{};
            }};
        cpuInfoParser.run();

        physicalCores = cpuIds.size();
    }

    /**
     * Get some details about the CPU
     */
    static void getCpuInfo(int& procCount, std::string& freq, std::string& features) {

        procCount = 0;

        CpuInfoParser cpuInfoParser{
            {
#ifdef __s390x__
                {R"re(processor\s+\d+)re", [&](const std::string& value) { procCount++; }},
                {"cpu MHz static", [&](const std::string& value) { freq = value; }},
                {"features", [&](const std::string& value) { features = value; }},
#else
                {"processor", [&](const std::string& value) { procCount++; }},
                {"cpu MHz", [&](const std::string& value) { freq = value; }},
                {"flags", [&](const std::string& value) { features = value; }},
#endif
            },
            []() {}};
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
        version += LinuxSysHelper::readLineFromFile("/proc/sys/kernel/osrelease");
    }

    /**
     * Get system memory total
     */
    static unsigned long long getSystemMemorySize() {
        std::string meminfo = readLineFromFile("/proc/meminfo");
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
            std::string groupLimit = readLineFromFile(file);
            if (!groupLimit.empty() && NumberParser{}(groupLimit, &groupMemBytes).isOK()) {
                return std::min(systemMemBytes, groupMemBytes);
            }
        }
        return systemMemBytes;
    }
};


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
}

/**
 * Save a BSON obj representing the host system's details
 */
void ProcessInfo::SystemInfo::collectSystemInfo() {
    utsname unameData;
    std::string distroName, distroVersion;
    std::string cpuFreq, cpuFeatures;
    int cpuCount;
    int physicalCores;

    std::string verSig = LinuxSysHelper::readLineFromFile("/proc/version_signature");
    LinuxSysHelper::getCpuInfo(cpuCount, cpuFreq, cpuFeatures);
    LinuxSysHelper::getNumPhysicalCores(physicalCores);
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
    pageSize = static_cast<unsigned long long>(sysconf(_SC_PAGESIZE));
    cpuArch = unameData.machine;
    hasNuma = checkNumaEnabled();

    BSONObjBuilder bExtra;
    bExtra.append("versionString", LinuxSysHelper::readLineFromFile("/proc/version"));
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
    bExtra.append("cpuFrequencyMHz", cpuFreq);
    bExtra.append("cpuFeatures", cpuFeatures);
    bExtra.append("pageSize", static_cast<long long>(pageSize));
    bExtra.append("numPages", static_cast<int>(sysconf(_SC_PHYS_PAGES)));
    bExtra.append("maxOpenFiles", static_cast<int>(sysconf(_SC_OPEN_MAX)));
    bExtra.append("physicalCores", physicalCores);

    appendMountInfo(bExtra);

    _extraStats = bExtra.obj();
}

/**
 * Determine if the process is running with (cc)NUMA
 */
bool ProcessInfo::checkNumaEnabled() {
    bool hasMultipleNodes = false;
    bool hasNumaMaps = false;

    try {
        hasMultipleNodes = boost::filesystem::exists("/sys/devices/system/node/node1");
        hasNumaMaps = boost::filesystem::exists("/proc/self/numa_maps");
    } catch (boost::filesystem::filesystem_error& e) {
        LOGV2(23340,
              "WARNING: Cannot detect if NUMA interleaving is enabled. Failed to probe",
              "path"_attr = e.path1().string(),
              "reason"_attr = e.code().message());
        return false;
    }

    if (hasMultipleNodes && hasNumaMaps) {
        // proc is populated with numa entries

        // read the second column of first line to determine numa state
        // ('default' = enabled, 'interleave' = disabled).  Logic from version.cpp's warnings.
        std::string line = LinuxSysHelper::readLineFromFile("/proc/self/numa_maps").append(" \0");
        size_t pos = line.find(' ');
        if (pos != std::string::npos &&
            line.substr(pos + 1, 10).find("interleave") == std::string::npos)
            // interleave not found;
            return true;
    }
    return false;
}

}  // namespace mongo
