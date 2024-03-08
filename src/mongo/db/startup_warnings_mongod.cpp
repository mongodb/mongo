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
#include <boost/move/utility_core.hpp>
#include <cstddef>
#include <fstream>  // IWYU pragma: keep
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/server_options.h"
#include "mongo/db/startup_warnings_common.h"
#include "mongo/db/startup_warnings_mongod.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/str.h"

#ifndef _WIN32
#include <sys/resource.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

using std::ios_base;
using std::string;
using namespace fmt::literals;

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

        auto numaPath = "/proc/self/numa_maps";
        std::ifstream f(numaPath, std::ifstream::in);
        if (f.is_open()) {
            std::string line;  // we only need the first line
            std::getline(f, line);
            if (f.fail()) {
                auto ec = lastSystemError();
                LOGV2_WARNING_OPTIONS(22200,
                                      {logv2::LogTag::kStartupWarnings},
                                      "Failed to read file",
                                      "filepath"_attr = numaPath,
                                      "error"_attr = errorMessage(ec));
            } else {
                // skip over pointer
                std::string::size_type where = line.find(' ');
                if ((where == std::string::npos) || (++where == line.size())) {
                    LOGV2_WARNING_OPTIONS(22165,
                                          {logv2::LogTag::kStartupWarnings},
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
}

#ifdef __linux__
void checkTHPSettings() {
    auto thpParameterPath = [](StringData parameter) -> std::string {
        return "{}/{}"_format(ProcessInfo::kTranparentHugepageDirectory, parameter);
    };

    // Transparent Hugepages checks
    StatusWith<std::string> transparentHugePagesEnabledResult =
        ProcessInfo::readTransparentHugePagesParameter("enabled");
    bool shouldWarnAboutDefrag = true;
    if (transparentHugePagesEnabledResult.isOK()) {
        StringData thpEnabledValue = transparentHugePagesEnabledResult.getValue();

#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
        if (thpEnabledValue != "always") {
            LOGV2_WARNING_OPTIONS(8640300,
                                  {logv2::LogTag::kStartupWarnings},
                                  "For customers running the tcmalloc-google memory allocator, we "
                                  "suggest setting the contents of sysfsFile to 'always'",
                                  "sysfsFile"_attr = thpParameterPath("enabled"),
                                  "currentValue"_attr = thpEnabledValue);
        }
#else   //  #ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
        if (thpEnabledValue == "always") {
            LOGV2_WARNING_OPTIONS(22178,
                                  {logv2::LogTag::kStartupWarnings},
                                  "For customers running the tcmalloc-gperftools memory allocator, "
                                  "we suggest setting the contents of sysfsFile to 'never'",
                                  "sysfsFile"_attr = thpParameterPath("enabled"),
                                  "currentValue"_attr = thpEnabledValue);
        } else {
            // If we do not have hugepages enabled and we do not want to have it enabled, we don't
            // need to warn about its features.
            shouldWarnAboutDefrag = false;
        }
#endif  // #ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
    } else if (transparentHugePagesEnabledResult.getStatus().code() !=
               ErrorCodes::NonExistentPath) {
        LOGV2_WARNING_OPTIONS(22202,
                              {logv2::LogTag::kStartupWarnings},
                              "Failed to read file",
                              "filepath"_attr = thpParameterPath("enabled"),
                              "error"_attr = transparentHugePagesEnabledResult.getStatus());
    }

    if (shouldWarnAboutDefrag) {
        StatusWith<std::string> transparentHugePagesDefragResult =
            ProcessInfo::readTransparentHugePagesParameter("defrag");
        if (transparentHugePagesDefragResult.isOK()) {
            auto defragValue = transparentHugePagesDefragResult.getValue();
#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
            if (defragValue != "defer+madvise") {
                LOGV2_WARNING_OPTIONS(
                    8640301,
                    {logv2::LogTag::kStartupWarnings},
                    "For customers running the updated tcmalloc-google memory allocator, we "
                    "suggest setting the contents of sysfsFile to 'defer+madvise'",
                    "sysfsFile"_attr = thpParameterPath("defrag"),
                    "currentValue"_attr = defragValue);
            }
#else   // #ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
            if (defragValue == "always") {
                LOGV2_WARNING_OPTIONS(
                    22181,
                    {logv2::LogTag::kStartupWarnings},
                    "For customers running the older tcmalloc-gperftools memory "
                    "allocator, we suggest setting the contents of sysfsFile to 'never'",
                    "sysfsFile"_attr = thpParameterPath("defrag"),
                    "currentValue"_attr = defragValue);
            }
#endif  // #ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
        } else if (transparentHugePagesDefragResult.getStatus().code() !=
                   ErrorCodes::NonExistentPath) {
            LOGV2_WARNING_OPTIONS(22204,
                                  {logv2::LogTag::kStartupWarnings},
                                  "Failed to read file",
                                  "filepath"_attr = thpParameterPath("defrag"),
                                  "error"_attr = transparentHugePagesDefragResult.getStatus());
        }
    }

#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
    auto maxPtesNonePath = thpParameterPath("khugepaged/max_ptes_none");
    std::fstream f(maxPtesNonePath, ios_base::in);
    unsigned maxPtesNoneValue;
    f >> maxPtesNoneValue;

    if (maxPtesNoneValue > 0) {
        LOGV2_WARNING_OPTIONS(8640302,
                              {logv2::LogTag::kStartupWarnings},
                              "We suggest setting the contents of sysfsFile to 0.",
                              "sysfsFile"_attr = maxPtesNonePath,
                              "currentValue"_attr = maxPtesNoneValue);
    }
#endif  // MONGO_CONFIG_TCMALLOC_GOOGLE
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

#ifdef __linux__
    if (boost::filesystem::exists("/proc/vz") && !boost::filesystem::exists("/proc/bc")) {
        LOGV2_OPTIONS(22161,
                      {logv2::LogTag::kStartupWarnings},
                      "You are running in OpenVZ which can cause issues on versions of RHEL older "
                      "than RHEL6");
    }

    checkMultipleNumaNodes();

    auto overcommitMemoryPath = "/proc/sys/vm/overcommit_memory";
    std::fstream f(overcommitMemoryPath, ios_base::in);
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
        std::fstream f(zoneReclaimModePath, ios_base::in);
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

    checkTHPSettings();

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

    // Check that swappiness is at a minimum (either 0 or 1)
    auto swappinessPath = "/proc/sys/vm/swappiness";
    if (boost::filesystem::exists(swappinessPath)) {
        std::fstream f(swappinessPath, ios_base::in);
        unsigned val;
        f >> val;
        if (val > 1) {
            LOGV2_WARNING_OPTIONS(8386700,
                                  {logv2::LogTag::kStartupWarnings},
                                  "We suggest setting swappiness to 0 or 1, as swapping can cause "
                                  "performance problems.",
                                  "sysfsFile"_attr = swappinessPath,
                                  "currentValue"_attr = val);
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
#endif  // #ifndef _WIN32

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
