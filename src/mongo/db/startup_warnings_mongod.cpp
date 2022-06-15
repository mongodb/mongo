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
#ifndef _WIN32
#include <sys/resource.h>
#endif

#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/startup_warnings_common.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/str.h"
#include "mongo/util/version.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {
namespace {

#define TRANSPARENT_HUGE_PAGES_DIR "/sys/kernel/mm/transparent_hugepage"

}  // namespace

using std::ios_base;
using std::string;

// static
StatusWith<std::string> StartupWarningsMongod::readTransparentHugePagesParameter(
    const std::string& parameter) {
    return readTransparentHugePagesParameter(parameter, TRANSPARENT_HUGE_PAGES_DIR);
}

// static
StatusWith<std::string> StartupWarningsMongod::readTransparentHugePagesParameter(
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

        std::string::size_type posBegin = line.find("[");
        std::string::size_type posEnd = line.find("]");
        if (posBegin == string::npos || posEnd == string::npos || posBegin >= posEnd) {
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
        if (opMode != "always" && opMode != "madvise" && opMode != "never") {
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

#ifdef __linux__
    if (boost::filesystem::exists("/proc/vz") && !boost::filesystem::exists("/proc/bc")) {
        LOGV2_OPTIONS(22161,
                      {logv2::LogTag::kStartupWarnings},
                      "You are running in OpenVZ which can cause issues on versions of RHEL older "
                      "than RHEL6");
    }

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
    if (hasMultipleNumaNodes) {
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
        if (f.is_open()) {
            std::string line;  // we only need the first line
            std::getline(f, line);
            if (f.fail()) {
                auto ec = lastSystemError();
                LOGV2_WARNING_OPTIONS(22200,
                                      {logv2::LogTag::kStartupWarnings},
                                      "Failed to read from /proc/self/numa_maps",
                                      "error"_attr = errorMessage(ec));
            } else {
                // skip over pointer
                std::string::size_type where = line.find(' ');
                if ((where == std::string::npos) || (++where == line.size())) {
                    LOGV2_WARNING_OPTIONS(22165,
                                          {logv2::LogTag::kStartupWarnings},
                                          "Cannot parse numa_maps at line: {line}",
                                          "Cannot parse numa_maps",
                                          "line"_attr = line);
                }
                // if the text following the space doesn't begin with 'interleave', then
                // issue the warning.
                else if (line.find("interleave", where) != where) {
                    LOGV2_WARNING_OPTIONS(22167,
                                          {logv2::LogTag::kStartupWarnings},
                                          "You are running on a NUMA machine. We suggest launching "
                                          "mongod like this to avoid performance problems: numactl "
                                          "--interleave=all mongod [other options]");
                }
            }
        }
    }

    std::fstream f("/proc/sys/vm/overcommit_memory", ios_base::in);
    unsigned val;
    f >> val;

    if (val == 2) {
        LOGV2_OPTIONS(22171,
                      {logv2::LogTag::kStartupWarnings},
                      "Journaling works best if /proc/sys/vm/overcommit_memory is set to 0 or 1",
                      "currentValue"_attr = val);
    }

    if (boost::filesystem::exists("/proc/sys/vm/zone_reclaim_mode")) {
        std::fstream f("/proc/sys/vm/zone_reclaim_mode", ios_base::in);
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

    // Transparent Hugepages checks
    StatusWith<std::string> transparentHugePagesEnabledResult =
        StartupWarningsMongod::readTransparentHugePagesParameter("enabled");
    bool shouldWarnAboutDefragAlways = false;
    if (transparentHugePagesEnabledResult.isOK()) {
        if (transparentHugePagesEnabledResult.getValue() == "always") {
            // If we do not have hugepages enabled, we don't need to warn about its features
            shouldWarnAboutDefragAlways = true;

            LOGV2_WARNING_OPTIONS(22178,
                                  {logv2::LogTag::kStartupWarnings},
                                  TRANSPARENT_HUGE_PAGES_DIR
                                  "/enabled is 'always'. We suggest setting it to 'never'");
        }
    } else if (transparentHugePagesEnabledResult.getStatus().code() !=
               ErrorCodes::NonExistentPath) {
        LOGV2_WARNING_OPTIONS(22202,
                              {logv2::LogTag::kStartupWarnings},
                              "Failed to read " TRANSPARENT_HUGE_PAGES_DIR "/enabled",
                              "error"_attr =
                                  transparentHugePagesEnabledResult.getStatus().reason());
    }

    StatusWith<std::string> transparentHugePagesDefragResult =
        StartupWarningsMongod::readTransparentHugePagesParameter("defrag");
    if (transparentHugePagesDefragResult.isOK()) {
        if (shouldWarnAboutDefragAlways &&
            transparentHugePagesDefragResult.getValue() == "always") {
            LOGV2_WARNING_OPTIONS(22181,
                                  {logv2::LogTag::kStartupWarnings},
                                  TRANSPARENT_HUGE_PAGES_DIR
                                  "/defrag is 'always'. We suggest setting it to 'never'");
        }
    } else if (transparentHugePagesDefragResult.getStatus().code() != ErrorCodes::NonExistentPath) {
        LOGV2_WARNING_OPTIONS(22204,
                              {logv2::LogTag::kStartupWarnings},
                              "Failed to read " TRANSPARENT_HUGE_PAGES_DIR "/defrag",
                              "error"_attr = transparentHugePagesDefragResult.getStatus().reason());
    }

    // Check if vm.max_map_count is high enough, as per SERVER-51233
    {
        size_t maxConns = svcCtx->getServiceEntryPoint()->maxOpenSessions();
        size_t requiredMapCount = 2 * maxConns;

        std::fstream f("/proc/sys/vm/max_map_count", ios_base::in);
        size_t val;
        f >> val;

        if (val < requiredMapCount) {
            LOGV2_WARNING_OPTIONS(5123300,
                                  {logv2::LogTag::kStartupWarnings},
                                  "** WARNING: Maximum number of memory map areas per process is "
                                  "too low. Current value of vm.max_map_count is {currentValue}, "
                                  "recommended minimum is {recommendedMinimum} for currently "
                                  "configured maximum connections ({maxConns})",
                                  "vm.max_map_count is too low",
                                  "currentValue"_attr = val,
                                  "recommendedMinimum"_attr = requiredMapCount,
                                  "maxConns"_attr = maxConns);
        }
    }
#endif  // __linux__

#ifndef _WIN32
    // Check that # of files rlmit >= 64000
    const unsigned int minNumFiles = 64000;
    struct rlimit rlnofile;

    if (!getrlimit(RLIMIT_NOFILE, &rlnofile)) {
        if (rlnofile.rlim_cur < minNumFiles) {
            LOGV2_WARNING_OPTIONS(22184,
                                  {logv2::LogTag::kStartupWarnings},
                                  "** WARNING: Soft rlimits too low. The limit for open file "
                                  "descriptors is {currentValue}, recommended "
                                  "minimum is {recommendedMinimum}",
                                  "Soft rlimits for open file descriptors too low",
                                  "currentValue"_attr = rlnofile.rlim_cur,
                                  "recommendedMinimum"_attr = minNumFiles);
        }
    } else {
        auto ec = lastSystemError();
        LOGV2_WARNING_OPTIONS(22186,
                              {logv2::LogTag::kStartupWarnings},
                              "getrlimit failed: {error}",
                              "getrlimit failed",
                              "error"_attr = errorMessage(ec));
    }

// Solaris does not have RLIMIT_MEMLOCK, these are exposed via getrctl(2) instead
#ifndef __sun
    // Check we can lock at least 16 pages for the SecureAllocator
    const unsigned int minLockedPages = 16;

    struct rlimit rlmemlock;

    if (!getrlimit(RLIMIT_MEMLOCK, &rlmemlock)) {
        if ((rlmemlock.rlim_cur / ProcessInfo::getPageSize()) < minLockedPages) {
            LOGV2_WARNING_OPTIONS(22188,
                                  {logv2::LogTag::kStartupWarnings},
                                  "** WARNING: Soft rlimits too low. The limit for locked memory "
                                  "size is {lockedMemoryBytes} "
                                  "bytes, it should be at least {minLockedMemoryBytes} bytes",
                                  "Soft rlimit for locked memory too low",
                                  "lockedMemoryBytes"_attr = rlmemlock.rlim_cur,
                                  "minLockedMemoryBytes"_attr =
                                      minLockedPages * ProcessInfo::getPageSize());
        }
    } else {
        auto ec = lastSystemError();
        LOGV2_WARNING_OPTIONS(22190,
                              {logv2::LogTag::kStartupWarnings},
                              "** WARNING: getrlimit failed: {error}",
                              "getrlimit failed",
                              "error"_attr = errorMessage(ec));
    }
#endif
#endif

#ifdef _WIN32
    ProcessInfo p;

    if (p.hasNumaEnabled()) {
        LOGV2_WARNING_OPTIONS(22192,
                              {logv2::LogTag::kStartupWarnings},
                              "You are running on a NUMA machine. We suggest disabling NUMA in the "
                              "machine BIOS by enabling interleaving to avoid performance "
                              "problems. See your BIOS documentation for more information");
    }

#endif  // #ifdef _WIN32

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
}
}  // namespace mongo
