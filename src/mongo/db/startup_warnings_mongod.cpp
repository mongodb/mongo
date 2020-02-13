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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

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
#include "mongo/util/log.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/str.h"
#include "mongo/util/version.h"

namespace mongo {
namespace {

const std::string kTransparentHugePagesDirectory("/sys/kernel/mm/transparent_hugepage");

}  // namespace

using std::ios_base;
using std::string;

// static
StatusWith<std::string> StartupWarningsMongod::readTransparentHugePagesParameter(
    const std::string& parameter) {
    return readTransparentHugePagesParameter(parameter, kTransparentHugePagesDirectory);
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
            int errorcode = errno;
            return StatusWith<std::string>(
                ErrorCodes::FileStreamFailed,
                str::stream() << "failed to read from " << filename << ": "
                              << ((ifs.eof()) ? "EOF" : errnoWithDescription(errorcode)));
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

    bool warned = false;

    if (sizeof(int*) == 4) {
        LOGV2_OPTIONS(22151, {logv2::LogTag::kStartupWarnings}, "");
        LOGV2_OPTIONS(
            22152, {logv2::LogTag::kStartupWarnings}, "** NOTE: This is a 32 bit MongoDB binary.");
        LOGV2_OPTIONS(22153,
                      {logv2::LogTag::kStartupWarnings},
                      "**       32 bit builds are limited to less than 2GB of data (or less with "
                      "--journal).");
        if (!storageParams.dur) {
            LOGV2_OPTIONS(
                22154,
                {logv2::LogTag::kStartupWarnings},
                "**       Note that journaling defaults to off for 32 bit and is currently off.");
        }
        LOGV2_OPTIONS(22155,
                      {logv2::LogTag::kStartupWarnings},
                      "**       See http://dochub.mongodb.org/core/32bit");
        warned = true;
    }

    if (!ProcessInfo::blockCheckSupported()) {
        LOGV2_OPTIONS(22156, {logv2::LogTag::kStartupWarnings}, "");
        LOGV2_OPTIONS(
            22157,
            {logv2::LogTag::kStartupWarnings},
            "** NOTE: your operating system version does not support the method that MongoDB");
        LOGV2_OPTIONS(22158,
                      {logv2::LogTag::kStartupWarnings},
                      "**       uses to detect impending page faults.");
        LOGV2_OPTIONS(22159,
                      {logv2::LogTag::kStartupWarnings},
                      "**       This may result in slower performance for certain use cases");
        warned = true;
    }
#ifdef __linux__
    if (boost::filesystem::exists("/proc/vz") && !boost::filesystem::exists("/proc/bc")) {
        LOGV2_OPTIONS(22160, {logv2::LogTag::kStartupWarnings}, "");
        LOGV2_OPTIONS(22161,
                      {logv2::LogTag::kStartupWarnings},
                      "** WARNING: You are running in OpenVZ which can cause issues on versions of "
                      "RHEL older than RHEL6.");
        warned = true;
    }

    bool hasMultipleNumaNodes = false;
    try {
        hasMultipleNumaNodes = boost::filesystem::exists("/sys/devices/system/node/node1");
    } catch (boost::filesystem::filesystem_error& e) {
        LOGV2_OPTIONS(22162, {logv2::LogTag::kStartupWarnings}, "");
        LOGV2_OPTIONS(22163,
                      {logv2::LogTag::kStartupWarnings},
                      "** WARNING: Cannot detect if NUMA interleaving is enabled. Failed to probe "
                      "\"{e_path1_string}\": {e_code_message}",
                      "e_path1_string"_attr = e.path1().string(),
                      "e_code_message"_attr = e.code().message());
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
                LOGV2_WARNING_OPTIONS(
                    22200,
                    {logv2::LogTag::kStartupWarnings},
                    "failed to read from /proc/self/numa_maps: {errnoWithDescription}",
                    "errnoWithDescription"_attr = errnoWithDescription());
                warned = true;
            } else {
                // skip over pointer
                std::string::size_type where = line.find(' ');
                if ((where == std::string::npos) || (++where == line.size())) {
                    LOGV2_OPTIONS(22164, {logv2::LogTag::kStartupWarnings}, "");
                    LOGV2_OPTIONS(22165,
                                  {logv2::LogTag::kStartupWarnings},
                                  "** WARNING: cannot parse numa_maps line: '{line}'",
                                  "line"_attr = line);
                    warned = true;
                }
                // if the text following the space doesn't begin with 'interleave', then
                // issue the warning.
                else if (line.find("interleave", where) != where) {
                    LOGV2_OPTIONS(22166, {logv2::LogTag::kStartupWarnings}, "");
                    LOGV2_OPTIONS(22167,
                                  {logv2::LogTag::kStartupWarnings},
                                  "** WARNING: You are running on a NUMA machine.");
                    LOGV2_OPTIONS(22168,
                                  {logv2::LogTag::kStartupWarnings},
                                  "**          We suggest launching mongod like this to avoid "
                                  "performance problems:");
                    LOGV2_OPTIONS(
                        22169,
                        {logv2::LogTag::kStartupWarnings},
                        "**              numactl --interleave=all mongod [other options]");
                    warned = true;
                }
            }
        }
    }

    if (storageParams.dur) {
        std::fstream f("/proc/sys/vm/overcommit_memory", ios_base::in);
        unsigned val;
        f >> val;

        if (val == 2) {
            LOGV2_OPTIONS(22170, {logv2::LogTag::kStartupWarnings}, "");
            LOGV2_OPTIONS(22171,
                          {logv2::LogTag::kStartupWarnings},
                          "** WARNING: /proc/sys/vm/overcommit_memory is {val}",
                          "val"_attr = val);
            LOGV2_OPTIONS(22172,
                          {logv2::LogTag::kStartupWarnings},
                          "**          Journaling works best with it set to 0 or 1");
        }
    }

    if (boost::filesystem::exists("/proc/sys/vm/zone_reclaim_mode")) {
        std::fstream f("/proc/sys/vm/zone_reclaim_mode", ios_base::in);
        unsigned val;
        f >> val;

        if (val != 0) {
            LOGV2_OPTIONS(22173, {logv2::LogTag::kStartupWarnings}, "");
            LOGV2_OPTIONS(22174,
                          {logv2::LogTag::kStartupWarnings},
                          "** WARNING: /proc/sys/vm/zone_reclaim_mode is {val}",
                          "val"_attr = val);
            LOGV2_OPTIONS(
                22175, {logv2::LogTag::kStartupWarnings}, "**          We suggest setting it to 0");
            LOGV2_OPTIONS(22176,
                          {logv2::LogTag::kStartupWarnings},
                          "**          http://www.kernel.org/doc/Documentation/sysctl/vm.txt");
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

            LOGV2_OPTIONS(22177, {logv2::LogTag::kStartupWarnings}, "");
            LOGV2_OPTIONS(22178,
                          {logv2::LogTag::kStartupWarnings},
                          "** WARNING: {kTransparentHugePagesDirectory}/enabled is 'always'.",
                          "kTransparentHugePagesDirectory"_attr = kTransparentHugePagesDirectory);
            LOGV2_OPTIONS(22179,
                          {logv2::LogTag::kStartupWarnings},
                          "**        We suggest setting it to 'never'");
            warned = true;
        }
    } else if (transparentHugePagesEnabledResult.getStatus().code() !=
               ErrorCodes::NonExistentPath) {
        LOGV2_WARNING_OPTIONS(22201, {logv2::LogTag::kStartupWarnings}, "");
        LOGV2_WARNING_OPTIONS(22202,
                              {logv2::LogTag::kStartupWarnings},
                              "{transparentHugePagesEnabledResult_getStatus_reason}",
                              "transparentHugePagesEnabledResult_getStatus_reason"_attr =
                                  transparentHugePagesEnabledResult.getStatus().reason());
        warned = true;
    }

    StatusWith<std::string> transparentHugePagesDefragResult =
        StartupWarningsMongod::readTransparentHugePagesParameter("defrag");
    if (transparentHugePagesDefragResult.isOK()) {
        if (shouldWarnAboutDefragAlways &&
            transparentHugePagesDefragResult.getValue() == "always") {
            LOGV2_OPTIONS(22180, {logv2::LogTag::kStartupWarnings}, "");
            LOGV2_OPTIONS(22181,
                          {logv2::LogTag::kStartupWarnings},
                          "** WARNING: {kTransparentHugePagesDirectory}/defrag is 'always'.",
                          "kTransparentHugePagesDirectory"_attr = kTransparentHugePagesDirectory);
            LOGV2_OPTIONS(22182,
                          {logv2::LogTag::kStartupWarnings},
                          "**        We suggest setting it to 'never'");
            warned = true;
        }
    } else if (transparentHugePagesDefragResult.getStatus().code() != ErrorCodes::NonExistentPath) {
        LOGV2_WARNING_OPTIONS(22203, {logv2::LogTag::kStartupWarnings}, "");
        LOGV2_WARNING_OPTIONS(22204,
                              {logv2::LogTag::kStartupWarnings},
                              "{transparentHugePagesDefragResult_getStatus_reason}",
                              "transparentHugePagesDefragResult_getStatus_reason"_attr =
                                  transparentHugePagesDefragResult.getStatus().reason());
        warned = true;
    }
#endif  // __linux__

#ifndef _WIN32
    // Check that # of files rlmit >= 64000
    const unsigned int minNumFiles = 64000;
    struct rlimit rlnofile;

    if (!getrlimit(RLIMIT_NOFILE, &rlnofile)) {
        if (rlnofile.rlim_cur < minNumFiles) {
            LOGV2_OPTIONS(22183, {logv2::LogTag::kStartupWarnings}, "");
            LOGV2_OPTIONS(22184,
                          {logv2::LogTag::kStartupWarnings},
                          "** WARNING: soft rlimits too low. Number of files is "
                          "{rlnofile_rlim_cur}, should be at least {minNumFiles}",
                          "rlnofile_rlim_cur"_attr = rlnofile.rlim_cur,
                          "minNumFiles"_attr = minNumFiles);
        }
    } else {
        const auto errmsg = errnoWithDescription();
        LOGV2_OPTIONS(22185, {logv2::LogTag::kStartupWarnings}, "");
        LOGV2_OPTIONS(22186,
                      {logv2::LogTag::kStartupWarnings},
                      "** WARNING: getrlimit failed. {errmsg}",
                      "errmsg"_attr = errmsg);
    }

// Solaris does not have RLIMIT_MEMLOCK, these are exposed via getrctl(2) instead
#ifndef __sun
    // Check we can lock at least 16 pages for the SecureAllocator
    const unsigned int minLockedPages = 16;

    struct rlimit rlmemlock;

    if (!getrlimit(RLIMIT_MEMLOCK, &rlmemlock)) {
        if ((rlmemlock.rlim_cur / ProcessInfo::getPageSize()) < minLockedPages) {
            LOGV2_OPTIONS(22187, {logv2::LogTag::kStartupWarnings}, "");
            LOGV2_OPTIONS(
                22188,
                {logv2::LogTag::kStartupWarnings},
                "** WARNING: soft rlimits too low. The locked memory size is {rlmemlock_rlim_cur} "
                "bytes, it should be at least {minLockedPages_ProcessInfo_getPageSize} bytes",
                "rlmemlock_rlim_cur"_attr = rlmemlock.rlim_cur,
                "minLockedPages_ProcessInfo_getPageSize"_attr =
                    minLockedPages * ProcessInfo::getPageSize());
        }
    } else {
        const auto errmsg = errnoWithDescription();
        LOGV2_OPTIONS(22189, {logv2::LogTag::kStartupWarnings}, "");
        LOGV2_OPTIONS(22190,
                      {logv2::LogTag::kStartupWarnings},
                      "** WARNING: getrlimit failed. {errmsg}",
                      "errmsg"_attr = errmsg);
    }
#endif
#endif

#ifdef _WIN32
    ProcessInfo p;

    if (p.hasNumaEnabled()) {
        LOGV2_OPTIONS(22191, {logv2::LogTag::kStartupWarnings}, "");
        LOGV2_OPTIONS(22192,
                      {logv2::LogTag::kStartupWarnings},
                      "** WARNING: You are running on a NUMA machine.");
        LOGV2_OPTIONS(22193,
                      {logv2::LogTag::kStartupWarnings},
                      "**          We suggest disabling NUMA in the machine BIOS ");
        LOGV2_OPTIONS(22194,
                      {logv2::LogTag::kStartupWarnings},
                      "**          by enabling interleaving to avoid performance problems. ");
        LOGV2_OPTIONS(22195,
                      {logv2::LogTag::kStartupWarnings},
                      "**          See your BIOS documentation for more information.");
        warned = true;
    }

#endif  // #ifdef _WIN32

    if (storageParams.engine == "ephemeralForTest") {
        LOGV2_OPTIONS(22196, {logv2::LogTag::kStartupWarnings}, "");
        LOGV2_OPTIONS(22197,
                      {logv2::LogTag::kStartupWarnings},
                      "** NOTE: The ephemeralForTest storage engine is for testing only. ");
        LOGV2_OPTIONS(
            22198, {logv2::LogTag::kStartupWarnings}, "**       Do not use in production.");
        warned = true;
    }

    if (warned) {
        LOGV2_OPTIONS(22199, {logv2::LogTag::kStartupWarnings}, "");
    }
}
}  // namespace mongo
