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

#include "mongo/db/startup_warnings_mongod.h"

#include <fstream>
#include <ios>

#include <boost/filesystem.hpp>
#include <fmt/format.h>

#ifdef __linux__
#include <linux/prctl.h>
#include <sys/prctl.h>
#endif  // __linux__

#ifndef _WIN32
#include <sys/resource.h>
#endif  // _WIN32

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/startup_warnings_common.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/procparser.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace {


#ifdef __linux__
#if MONGO_CONFIG_TCMALLOC_GOOGLE
constexpr bool kUsingGoogleTCMallocAllocator = true;
auto kAllocatorName = "tcmalloc-google"_sd;
#elif MONGO_CONFIG_TCMALLOC_GPERF
constexpr bool kUsingGoogleTCMallocAllocator = false;
auto kAllocatorName = "tcmalloc-gperftools"_sd;
#else
constexpr bool kUsingGoogleTCMallocAllocator = false;
auto kAllocatorName = "system"_sd;
#endif  // MONGO_CONFIG_TCMALLOC_GOOGLE

#endif  // __linux__

#ifdef _WIN32
void logWinMongodWarnings(const StorageGlobalParams& storageParams,
                          const ServerGlobalParams& serverParams,
                          ServiceContext* svcCtx) {
    ProcessInfo p;

    if (p.hasNumaEnabled()) {
        LOGV2_WARNING_OPTIONS(22192,
                              {logv2::LogTag::kStartupWarnings},
                              "You are running on a NUMA machine. We suggest disabling NUMA in the "
                              "machine BIOS by enabling interleaving to avoid performance "
                              "problems. See your BIOS documentation for more information");
    }
}

#else  // not _WIN32
void logNonWinMongodWarnings(const StorageGlobalParams& storageParams,
                             const ServerGlobalParams& serverParams,
                             ServiceContext* svcCtx) {
    // Check that # of files rlmit >= 64000
    const unsigned int minNumFiles = 64000;
    struct rlimit rlnofile;

    if (!getrlimit(RLIMIT_NOFILE, &rlnofile)) {
        if (rlnofile.rlim_cur < minNumFiles) {
            LOGV2_WARNING_OPTIONS(22184,
                                  {logv2::LogTag::kStartupWarnings},
                                  "Soft rlimits for open file descriptors too low",
                                  "currentValue"_attr = rlnofile.rlim_cur,
                                  "recommendedMinimum"_attr = minNumFiles);
        }
    } else {
        auto ec = lastSystemError();
        LOGV2_WARNING_OPTIONS(22186,
                              {logv2::LogTag::kStartupWarnings},
                              "getrlimit failed",
                              "error"_attr = errorMessage(ec));
    }

    // Check we can lock at least 16 pages for the SecureAllocator
    const unsigned int minLockedPages = 16;

    struct rlimit rlmemlock;

    if (!getrlimit(RLIMIT_MEMLOCK, &rlmemlock)) {
        if ((rlmemlock.rlim_cur / ProcessInfo::getPageSize()) < minLockedPages) {
            LOGV2_WARNING_OPTIONS(22188,
                                  {logv2::LogTag::kStartupWarnings},
                                  "Soft rlimit for locked memory too low",
                                  "lockedMemoryBytes"_attr = rlmemlock.rlim_cur,
                                  "minLockedMemoryBytes"_attr =
                                      minLockedPages * ProcessInfo::getPageSize());
        }
    } else {
        auto ec = lastSystemError();
        LOGV2_WARNING_OPTIONS(22190,
                              {logv2::LogTag::kStartupWarnings},
                              "getrlimit failed",
                              "error"_attr = errorMessage(ec));
    }
}

#endif  // _WIN32

#ifdef __linux__

bool isSwapTotalNonZeroInProcMemInfo() {
    const auto memInfoPath = "/proc/meminfo"_sd;
    BSONObjBuilder b;
    uassertStatusOK(procparser::parseProcMemInfoFile(memInfoPath, {"SwapTotal"_sd}, &b));
    BSONObj obj = b.done();
    uassert(ErrorCodes::FailedToParse,
            "SwapTotal not found in /proc/meminfo",
            obj.hasField("SwapTotal_kb"));
    return obj["SwapTotal_kb"].trueValue();
}

void checkMultipleNumaNodes() {
    bool hasMultipleNumaNodes = false;

    try {
        hasMultipleNumaNodes = boost::filesystem::exists("/sys/devices/system/node/node1");
    } catch (boost::filesystem::filesystem_error& e) {
        LOGV2_WARNING_OPTIONS(22163,
                              {logv2::LogTag::kStartupWarnings},
                              "Cannot detect if NUMA interleaving is enabled. Failed to probe path",
                              "path"_attr = e.path1().string(),
                              "error"_attr = errorMessage(e.code()));
    }

    if (!hasMultipleNumaNodes) {
        return;
    }

    // We are on a box with a NUMA enabled kernel and more than 1 numa node (they start at
    // node0)
    // Now we look at the first line of /proc/self/numa_maps
    //
    // Bad example:
    // $ cat /proc/self/numa_maps
    // 00400000 default file=/bin/cat mapped=6 N4=6
    //
    // Good example:
    // $ numactl --interleave=all cat /proc/self/numa_maps
    // 00400000 interleave:0-7 file=/bin/cat mapped=6 N4=6

    auto numaPath = "/proc/self/numa_maps";
    std::ifstream f(numaPath, std::ifstream::in);
    if (!f.is_open()) {
        return;
    }

    std::string line;  // we only need the first line
    std::getline(f, line);
    if (f.fail()) {
        auto ec = lastSystemError();
        LOGV2_WARNING_OPTIONS(22200,
                              {logv2::LogTag::kStartupWarnings},
                              "Failed to read file",
                              "filepath"_attr = numaPath,
                              "error"_attr = errorMessage(ec));
        return;
    }

    // skip over pointer
    std::string::size_type where = line.find(' ');
    if ((where == std::string::npos) || (++where == line.size())) {
        LOGV2_WARNING_OPTIONS(
            22165, {logv2::LogTag::kStartupWarnings}, "Cannot parse numa_maps", "line"_attr = line);
    }

    else if (line.find("interleave", where) != where) {
        // The text following the space doesn't begin with 'interleave', so issue the warning.
        LOGV2_WARNING_OPTIONS(22167,
                              {logv2::LogTag::kStartupWarnings},
                              "You are running on a NUMA machine. We suggest launching "
                              "mongod like this to avoid performance problems: numactl "
                              "--interleave=all mongod [other options]");
    }
}

std::string thpParameterPath(StringData parameter) {
    return fmt::format("{}/{}", ProcessInfo::kTranparentHugepageDirectory, parameter);
}

void logIncorrectAllocatorSettings(StringData path,
                                   StringData desiredValue,
                                   StringData currentValue) {
    LOGV2_WARNING_OPTIONS(
        9068900,
        {logv2::LogTag::kStartupWarnings},
        "For customers running the current memory allocator, we suggest changing the contents "
        "of the following sysfsFile",
        "allocator"_attr = kAllocatorName,
        "sysfsFile"_attr = path,
        "currentValue"_attr = currentValue,
        "desiredValue"_attr = desiredValue);
}

void warnTHPWronglyDisabledOnSystem() {
    logIncorrectAllocatorSettings(thpParameterPath("enabled"), "always", "never");
}

void warnTHPWronglyEnabledOnSystem() {
    logIncorrectAllocatorSettings(thpParameterPath("enabled"), "never", "always");
}

void warnTHPWronglyDisabledOnProcess() {
    LOGV2_WARNING_OPTIONS(
        9068901,
        {logv2::LogTag::kStartupWarnings},
        "For customers running the current memory allocator, we suggest re-enabling transparent "
        "hugepages for this process via setting PR_SET_THP_DISABLE to 0.",
        "allocator"_attr = kAllocatorName);
}

void warnTHPSystemValueError(const Status& status) {
    LOGV2_WARNING_OPTIONS(22202,
                          {logv2::LogTag::kStartupWarnings},
                          "Failed to read file",
                          "filepath"_attr = thpParameterPath("enabled"),
                          "error"_attr = status);
}

void warnTHPOptOutError(const std::error_code& errorCode) {
    LOGV2_WARNING_OPTIONS(9068902,
                          {logv2::LogTag::kStartupWarnings},
                          "Unable to tell whether transparent hugepages are disabled for this "
                          "process via prctl, as a result unable to verify the state of "
                          "transparent hugepages on this system.",
                          "errorMessage"_attr = errorMessage(errorCode));
}

unsigned retrieveMaxPtesNoneValue() {
    auto maxPtesNonePath = thpParameterPath("khugepaged/max_ptes_none");
    std::fstream f(maxPtesNonePath, std::ios_base::in);
    unsigned maxPtesNoneValue;
    f >> maxPtesNoneValue;
    return maxPtesNoneValue;
}

std::variant<std::error_code, bool> checkOptingOutOfTHPForProcess() {
    int optingOutForProcess = prctl(PR_GET_THP_DISABLE, 0, 0, 0, 0);
    if (optingOutForProcess == -1) {
        return lastSystemError();
    }
    return !!optingOutForProcess;
}

#endif  // __linux__

}  // namespace

#ifdef __linux__
namespace startup_warning_detail {
bool verifyMaxPtesNoneIsCorrect(bool usingGoogleTCMallocAllocator, unsigned value) {
    return !usingGoogleTCMallocAllocator || value == 0;
}

THPEnablementWarningLogCase getTHPEnablementWarningCase(
    bool usingGoogleTCMallocAllocator,
    const StatusWith<std::string>& thpEnabled,
    const std::variant<std::error_code, bool>& optingOutOfTHPForProcess) {
    if (!thpEnabled.isOK()) {
        if (const auto* optingOut = std::get_if<bool>(&optingOutOfTHPForProcess)) {
            if (usingGoogleTCMallocAllocator && *optingOut) {
                return THPEnablementWarningLogCase::kSystemValueErrorWithWrongOptOut;
            } else {
                return THPEnablementWarningLogCase::kSystemValueError;
            }
        } else {
            return THPEnablementWarningLogCase::kSystemValueErrorWithOptOutError;
            ;
        }
    }

    if (usingGoogleTCMallocAllocator) {
        if (thpEnabled.getValue() != "always") {
            return THPEnablementWarningLogCase::kWronglyDisabledOnSystem;
        }

        if (const auto* optingOut = std::get_if<bool>(&optingOutOfTHPForProcess)) {
            if (*optingOut) {
                return THPEnablementWarningLogCase::kWronglyDisabledViaOptOut;
            }
        } else {
            return THPEnablementWarningLogCase::kOptOutError;
        }
    } else {
        if (thpEnabled.getValue() != "always") {
            return THPEnablementWarningLogCase::kNone;
        }

        if (const auto* optingOut = std::get_if<bool>(&optingOutOfTHPForProcess)) {
            if (!*optingOut) {
                return THPEnablementWarningLogCase::kWronglyEnabled;
            }
        } else {
            return THPEnablementWarningLogCase::kOptOutError;
        }
    }

    return THPEnablementWarningLogCase::kNone;
}

THPDefragWarningLogCase getDefragWarningCase(bool usingGoogleTCMallocAllocator,
                                             const StatusWith<std::string>& thpDefragSettings) {
    if (!usingGoogleTCMallocAllocator) {
        return THPDefragWarningLogCase::kNone;
    }

    if (!thpDefragSettings.isOK()) {
        return THPDefragWarningLogCase::kError;
    }

    if (thpDefragSettings.getValue() != "defer+madvise") {
        return THPDefragWarningLogCase::kWronglyNotUsingDeferMadvise;
    }

    return THPDefragWarningLogCase::kNone;
}

void warnForTHPEnablementCases(
    THPEnablementWarningLogCase warningCase,
    const StatusWith<std::string>& thpEnabled,
    const std::variant<std::error_code, bool>& optingOutOfTHPForProcess) {

    switch (warningCase) {
        case THPEnablementWarningLogCase::kWronglyEnabled:
            warnTHPWronglyEnabledOnSystem();
            break;
        case THPEnablementWarningLogCase::kWronglyDisabledViaOptOut:
        case THPEnablementWarningLogCase::kSystemValueErrorWithWrongOptOut:
            warnTHPWronglyDisabledOnProcess();
            break;
        case THPEnablementWarningLogCase::kWronglyDisabledOnSystem:
            warnTHPWronglyDisabledOnSystem();
            break;
        case THPEnablementWarningLogCase::kSystemValueError:
            warnTHPSystemValueError(thpEnabled.getStatus());
            break;
        case THPEnablementWarningLogCase::kOptOutError:
            warnTHPOptOutError(std::get<std::error_code>(optingOutOfTHPForProcess));
            break;
        case THPEnablementWarningLogCase::kSystemValueErrorWithOptOutError:
            warnTHPSystemValueError(thpEnabled.getStatus());
            warnTHPOptOutError(std::get<std::error_code>(optingOutOfTHPForProcess));
            break;
        case THPEnablementWarningLogCase::kNone:
            break;
    }
}


void warnForDefragCase(THPDefragWarningLogCase warningCase,
                       const StatusWith<std::string>& thpDefragSettings) {
    if (warningCase == THPDefragWarningLogCase::kError) {
        LOGV2_WARNING_OPTIONS(22204,
                              {logv2::LogTag::kStartupWarnings},
                              "Failed to read file",
                              "filepath"_attr = thpParameterPath("defrag"),
                              "error"_attr = thpDefragSettings.getStatus());
    } else if (warningCase == THPDefragWarningLogCase::kWronglyNotUsingDeferMadvise) {
        logIncorrectAllocatorSettings(
            thpParameterPath("defrag"), "defer+madvise", thpDefragSettings.getValue());
    }
}

void verifyCorrectTHPSettings(bool usingGoogleTCMallocAllocator,
                              const StatusWith<std::string>& thpEnabled,
                              const std::variant<std::error_code, bool>& optingOutOfTHPForProcess,
                              const StatusWith<std::string>& thpDefragSettings,
                              unsigned maxPtesNoneValue) {
    auto wCase = getTHPEnablementWarningCase(
        usingGoogleTCMallocAllocator, thpEnabled, optingOutOfTHPForProcess);
    warnForTHPEnablementCases(wCase, thpEnabled, optingOutOfTHPForProcess);

    auto defragCase = getDefragWarningCase(usingGoogleTCMallocAllocator, thpDefragSettings);
    warnForDefragCase(defragCase, thpDefragSettings);

    if (!verifyMaxPtesNoneIsCorrect(usingGoogleTCMallocAllocator, maxPtesNoneValue)) {
        LOGV2_WARNING_OPTIONS(8640302,
                              {logv2::LogTag::kStartupWarnings},
                              "We suggest setting the contents of sysfsFile to 0.",
                              "sysfsFile"_attr = thpParameterPath("khugepaged/max_ptes_none"),
                              "currentValue"_attr = maxPtesNoneValue);
    }
}

void logLinuxMongodWarnings(const StorageGlobalParams& storageParams,
                            const ServerGlobalParams& serverParams,
                            ServiceContext* svcCtx) {
    if (boost::filesystem::exists("/proc/vz") && !boost::filesystem::exists("/proc/bc")) {
        LOGV2_OPTIONS(22161,
                      {logv2::LogTag::kStartupWarnings},
                      "You are running in OpenVZ which can cause issues on versions of RHEL older "
                      "than RHEL6");
    }

    checkMultipleNumaNodes();

    auto overcommitMemoryPath = "/proc/sys/vm/overcommit_memory";
    std::fstream f(overcommitMemoryPath, std::ios_base::in);
    unsigned val;
    f >> val;

    if (val == 2) {
        LOGV2_OPTIONS(22171,
                      {logv2::LogTag::kStartupWarnings},
                      "Journaling and memory allocation work best if overcommit_memory is set to 1",
                      "sysfsFile"_attr = overcommitMemoryPath,
                      "currentValue"_attr = val);
    }

    auto zoneReclaimModePath = "/proc/sys/vm/zone_reclaim_mode";
    if (boost::filesystem::exists(zoneReclaimModePath)) {
        std::fstream f(zoneReclaimModePath, std::ios_base::in);
        unsigned val;
        f >> val;

        if (val != 0) {
            LOGV2_OPTIONS(22174,
                          {logv2::LogTag::kStartupWarnings},
                          "We suggest setting zone_reclaim_mode to 0. See "
                          "http://www.kernel.org/doc/Documentation/sysctl/vm.txt",
                          "sysfsFile"_attr = zoneReclaimModePath,
                          "currentValue"_attr = val);
        }
    }

    verifyCorrectTHPSettings(kUsingGoogleTCMallocAllocator,
                             ProcessInfo::readTransparentHugePagesParameter("enabled"),
                             checkOptingOutOfTHPForProcess(),
                             ProcessInfo::readTransparentHugePagesParameter("defrag"),
                             retrieveMaxPtesNoneValue());

#if defined(MONGO_CONFIG_TCMALLOC_GOOGLE) && defined(MONGO_CONFIG_GLIBC_RSEQ)
    if (auto res = ProcessInfo::checkGlibcRseqTunable(); !res) {
        LOGV2_WARNING_OPTIONS(
            8718500,
            {logv2::LogTag::kStartupWarnings},
            "Your system has glibc support for rseq built in, which is not yet supported by "
            "tcmalloc-google and has critical performance implications. Please set the "
            "environment variable GLIBC_TUNABLES=glibc.pthread.rseq=0");
    }
#endif

    if (auto tlm = svcCtx->getTransportLayerManager()) {
        tlm->checkMaxOpenSessionsAtStartup();
    }

    // Check that swappiness is at a minimum (either 0 or 1) if swap is enabled
    bool swapEnabled = false;
    try {
        swapEnabled = isSwapTotalNonZeroInProcMemInfo();
    } catch (DBException& e) {
        LOGV2_WARNING_OPTIONS(8949500,
                              {logv2::LogTag::kStartupWarnings},
                              "Failed to parse /proc/meminfo",
                              "error"_attr = e.toStatus());
    }

    if (swapEnabled) {
        auto swappinessPath = "/proc/sys/vm/swappiness";
        if (boost::filesystem::exists(swappinessPath)) {
            std::fstream f(swappinessPath, std::ios_base::in);
            unsigned val;
            f >> val;
            if (val > 1) {
                LOGV2_WARNING_OPTIONS(
                    8386700,
                    {logv2::LogTag::kStartupWarnings},
                    "We suggest setting swappiness to 0 or 1, as swapping can cause "
                    "performance problems.",
                    "sysfsFile"_attr = swappinessPath,
                    "currentValue"_attr = val);
            }
        }
    }
}

}  // namespace startup_warning_detail

#endif  // __linux__

void logMongodStartupWarnings(const StorageGlobalParams& storageParams,
                              const ServerGlobalParams& serverParams,
                              ServiceContext* svcCtx) {
    logCommonStartupWarnings(serverParams);

    if (sizeof(int*) == 4) {
        LOGV2_WARNING_OPTIONS(
            22152,
            {logv2::LogTag::kStartupWarnings},
            "This is a 32 bit MongoDB binary. 32 bit builds are limited to less than 2GB "
            "of data. See http://dochub.mongodb.org/core/32bit");
    }

#ifdef _WIN32
    logWinMongodWarnings(storageParams, serverParams, svcCtx);
#else
    logNonWinMongodWarnings(storageParams, serverParams, svcCtx);
#endif

#ifdef __linux__
    startup_warning_detail::logLinuxMongodWarnings(storageParams, serverParams, svcCtx);
#endif

    if (storageParams.restore) {
        LOGV2_OPTIONS(
            6260401,
            {logv2::LogTag::kStartupWarnings},
            "Running with --restore. This should only be used when restoring from a backup");
    }

    if (repl::ReplSettings::shouldRecoverFromOplogAsStandalone()) {
        LOGV2_WARNING_OPTIONS(21558,
                              {logv2::LogTag::kStartupWarnings},
                              "Setting mongod to readOnly mode as a result of specifying "
                              "'recoverFromOplogAsStandalone'");
    }

    if (storageParams.magicRestore) {
        LOGV2_OPTIONS(8892401,
                      {logv2::LogTag::kStartupWarnings},
                      "Running with --magicRestore. This should only be used when restoring from a "
                      "backup using magic restore.");
    }
}

}  // namespace mongo
