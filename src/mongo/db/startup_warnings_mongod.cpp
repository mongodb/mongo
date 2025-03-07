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

#include "mongo/db/startup_warnings_mongod.h"

#include <boost/filesystem/operations.hpp>
#include <fstream>

#ifdef __linux__
#include <linux/prctl.h>
#include <sys/prctl.h>
#endif  // __linux__

#ifndef _WIN32
#include <sys/resource.h>
#endif  // _WIN32

#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/startup_warnings_common.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace {

using namespace fmt::literals;

#ifdef __linux__
#define TRANSPARENT_HUGE_PAGES_DIR "/sys/kernel/mm/transparent_hugepage"
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
}

#ifndef __sun
// The below calls are only available on non-Solaris systems, as Solaris does not have
// RLIMIT_MEMLOCK.
void logNonWinNonSunMongodWarnings() {
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
#endif  // not __sun
#endif  // not _WIN32

#ifdef __linux__

void checkMultipleNumaNodes() {
    bool hasMultipleNumaNodes = false;
    try {
        hasMultipleNumaNodes = boost::filesystem::exists("/sys/devices/system/node/node1");
    } catch (boost::filesystem::filesystem_error& e) {
        LOGV2_WARNING_OPTIONS(22163,
                              {logv2::LogTag::kStartupWarnings},
                              "Cannot detect if NUMA interleaving is enabled. Failed to probe path",
                              "path"_attr = e.path1().string(),
                              "error"_attr = e.code().message());
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

    std::ifstream f("/proc/self/numa_maps", std::ifstream::in);
    if (!f.is_open()) {
        return;
    }

    std::string line;  // we only need the first line
    std::getline(f, line);
    if (f.fail()) {
        auto ec = lastSystemError();
        LOGV2_WARNING_OPTIONS(22200,
                              {logv2::LogTag::kStartupWarnings},
                              "Failed to read from /proc/self/numa_maps",
                              "error"_attr = errorMessage(ec));
        return;
    }

    // skip over pointer
    auto where = line.find(' ');
    if ((where == std::string::npos) || (++where == line.size())) {
        LOGV2_WARNING_OPTIONS(
            22165, {logv2::LogTag::kStartupWarnings}, "Cannot parse numa_maps", "line"_attr = line);
    } else if (line.find("interleave", where) != where) {
        // The text following the space doesn't begin with 'interleave', so issue the warning.
        LOGV2_WARNING_OPTIONS(22167,
                              {logv2::LogTag::kStartupWarnings},
                              "You are running on a NUMA machine. We suggest launching "
                              "mongod like this to avoid performance problems: numactl "
                              "--interleave=all mongod [other options]");
    }
}

inline bool isValidOpMode(const std::string& opMode, const std::string& param) noexcept {
    if (param == kTHPEnabledParameter) {
        return opMode == "always" || opMode == "madvise" || opMode == "never";
    }
    return false;
}

void warnTHPWronglyEnabledOnSystem() {
    LOGV2_WARNING_OPTIONS(9068900,
                          {logv2::LogTag::kStartupWarnings},
                          "For customers running MongoDB 7.0, we suggest changing the contents "
                          "of the following sysfsFile",
                          "sysfsFile"_attr = TRANSPARENT_HUGE_PAGES_DIR,
                          "currentValue"_attr = "always",
                          "desiredValue"_attr = "never");
}

void warnTHPSystemValueError(const Status& status) {
    LOGV2_WARNING_OPTIONS(22202,
                          {logv2::LogTag::kStartupWarnings},
                          "Failed to read file",
                          "filepath"_attr = TRANSPARENT_HUGE_PAGES_DIR,
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

StatusWith<std::string> StartupWarningsMongodLinux::readTransparentHugePagesParameter(
    const std::string& parameter) {
    return readTransparentHugePagesParameter(parameter, TRANSPARENT_HUGE_PAGES_DIR);
}

StatusWith<std::string> StartupWarningsMongodLinux::readTransparentHugePagesParameter(
    const std::string& parameter, const std::string& directory) {
    std::string opMode;
    try {
        boost::filesystem::path directoryPath(directory);
        if (!boost::filesystem::exists(directoryPath)) {
            return StatusWith<std::string>(
                ErrorCodes::NonExistentPath,
                str::stream() << "Unable to read non-existent transparent Huge Pages directory: "
                              << directory);
        }

        boost::filesystem::path parameterPath(directoryPath / parameter);
        if (!boost::filesystem::exists(parameterPath)) {
            return StatusWith<std::string>(
                ErrorCodes::NonExistentPath,
                str::stream() << "Unable to read non-existent transparent Huge Pages file: "
                              << parameterPath.string());
        }

        std::string filename(parameterPath.string());
        std::ifstream ifs(filename.c_str());
        if (!ifs) {
            return StatusWith<std::string>(
                ErrorCodes::FileNotOpen,
                str::stream() << "Unable to open transparent Huge Pages file " << filename);
        }

        std::string line;
        if (!std::getline(ifs, line)) {
            auto ec = lastSystemError();
            return StatusWith<std::string>(ErrorCodes::FileStreamFailed,
                                           str::stream()
                                               << "failed to read from " << filename << ": "
                                               << ((ifs.eof()) ? "EOF" : errorMessage(ec)));
        }

        std::string::size_type posBegin = line.find('[');
        std::string::size_type posEnd = line.find(']');
        if (posBegin == std::string::npos || posEnd == std::string::npos || posBegin >= posEnd) {
            return StatusWith<std::string>(ErrorCodes::FailedToParse,
                                           str::stream() << "cannot parse line: '" << line << "'");
        }

        opMode = line.substr(posBegin + 1, posEnd - posBegin - 1);
        if (opMode.empty()) {
            return StatusWith<std::string>(ErrorCodes::BadValue,
                                           str::stream() << "invalid mode in " << filename << ": '"
                                                         << line << "'");
        }

        // Check against acceptable values of opMode.
        if (!isValidOpMode(opMode, parameter)) {
            return StatusWith<std::string>(
                ErrorCodes::BadValue,
                str::stream()
                    << "** WARNING: unrecognized transparent Huge Pages mode of operation in "
                    << filename << ": '" << opMode << "''");
        }
    } catch (const boost::filesystem::filesystem_error& err) {
        return StatusWith<std::string>(ErrorCodes::UnknownError,
                                       str::stream() << "Failed to probe \"" << err.path1().string()
                                                     << "\": " << err.code().message());
    }

    return StatusWith<std::string>(opMode);
}

auto StartupWarningsMongodLinux::getTHPEnablementWarningCase(
    const StatusWith<std::string>& thpEnabled,
    const std::variant<std::error_code, bool>& optingOutOfTHPForProcess)
    -> THPEnablementWarningLogCase {
    using WLC = THPEnablementWarningLogCase;
    if (!thpEnabled.isOK()) {
        if (std::get_if<bool>(&optingOutOfTHPForProcess)) {
            return WLC::kSystemValueError;
        } else {
            return WLC::kSystemValueErrorWithOptOutError;
        }
    }

    if (thpEnabled.getValue() != "always") {
        return WLC::kNone;
    }

    const auto* optingOut = std::get_if<bool>(&optingOutOfTHPForProcess);
    if (!optingOut) {
        return WLC::kOptOutError;
    }

    if (!*optingOut) {
        return WLC::kWronglyEnabled;
    }

    return WLC::kNone;
}

void StartupWarningsMongodLinux::warnForTHPEnablementCases(
    THPEnablementWarningLogCase warningCase,
    const StatusWith<std::string>& thpEnabled,
    const std::variant<std::error_code, bool>& optingOutOfTHPForProcess) {

    switch (warningCase) {
        case THPEnablementWarningLogCase::kWronglyEnabled:
            warnTHPWronglyEnabledOnSystem();
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

void StartupWarningsMongodLinux::verifyCorrectTHPSettings(
    const StatusWith<std::string>& thpEnabled,
    const std::variant<std::error_code, bool>& optingOutOfTHPForProcess) {
    auto wCase = getTHPEnablementWarningCase(thpEnabled, optingOutOfTHPForProcess);
    warnForTHPEnablementCases(wCase, thpEnabled, optingOutOfTHPForProcess);
}

void StartupWarningsMongodLinux::logLinuxMongodWarnings(const StorageGlobalParams& storageParams,
                                                        const ServerGlobalParams& serverParams,
                                                        ServiceContext* svcCtx) {
    if (boost::filesystem::exists("/proc/vz") && !boost::filesystem::exists("/proc/bc")) {
        LOGV2_OPTIONS(22161,
                      {logv2::LogTag::kStartupWarnings},
                      "You are running in OpenVZ which can cause issues on versions of RHEL older "
                      "than RHEL6");
    }

    checkMultipleNumaNodes();

    std::fstream f("/proc/sys/vm/overcommit_memory", std::ios_base::in);
    unsigned val;
    f >> val;

    if (val == 2) {
        LOGV2_OPTIONS(22171,
                      {logv2::LogTag::kStartupWarnings},
                      "Journaling works best if /proc/sys/vm/overcommit_memory is set to 0 or 1",
                      "currentValue"_attr = val);
    }

    if (boost::filesystem::exists("/proc/sys/vm/zone_reclaim_mode")) {
        std::fstream f("/proc/sys/vm/zone_reclaim_mode", std::ios_base::in);
        unsigned val;
        f >> val;

        if (val != 0) {
            LOGV2_OPTIONS(22174,
                          {logv2::LogTag::kStartupWarnings},
                          "We suggest setting /proc/sys/vm/zone_reclaim_mode to 0. See "
                          "http://www.kernel.org/doc/Documentation/sysctl/vm.txt",
                          "currentValue"_attr = val);
        }
    }

    verifyCorrectTHPSettings(
        StartupWarningsMongodLinux::readTransparentHugePagesParameter(kTHPEnabledParameter),
        checkOptingOutOfTHPForProcess());

    // Check if vm.max_map_count is high enough, as per SERVER-51233
    {
        size_t maxConns = svcCtx->getServiceEntryPoint()->maxOpenSessions();
        size_t requiredMapCount = 2 * maxConns;

        std::fstream f("/proc/sys/vm/max_map_count", std::ios_base::in);
        size_t val;
        f >> val;

        if (val < requiredMapCount) {
            LOGV2_WARNING_OPTIONS(5123300,
                                  {logv2::LogTag::kStartupWarnings},
                                  "vm.max_map_count is too low",
                                  "currentValue"_attr = val,
                                  "recommendedMinimum"_attr = requiredMapCount,
                                  "maxConns"_attr = maxConns);
        }
    }
}

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

#ifndef __sun
    logNonWinNonSunMongodWarnings();
#endif  // __sun
#endif  // _WIN32

#ifdef __linux__
    StartupWarningsMongodLinux::logLinuxMongodWarnings(storageParams, serverParams, svcCtx);
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

    const auto kFrameworkControl =
        ServerParameterSet::getNodeParameterSet()->get<QueryFrameworkControl>(
            "internalQueryFrameworkControl");
    if (kFrameworkControl &&
        kFrameworkControl->_data.get() != QueryFrameworkControlEnum::kForceClassicEngine) {
        LOGV2_WARNING_OPTIONS(9473500,
                              {logv2::LogTag::kStartupWarnings},
                              "'internalQueryFrameworkControl' is set to a non-default value. SBE "
                              "is no longer a supported engine in version 7.0, and should not be "
                              "used in production environments. Please set "
                              "'internalQueryFrameworkControl' to 'forceClassicEngine'");
    }
}
}  // namespace mongo
