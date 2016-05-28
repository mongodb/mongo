/**
*    Copyright (C) 2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/db/startup_warnings_mongod.h"

#include <boost/filesystem/operations.hpp>
#include <fstream>
#ifndef _WIN32
#include <sys/resource.h>
#endif

#include "mongo/db/server_options.h"
#include "mongo/db/startup_warnings_common.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/processinfo.h"
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
            return StatusWith<std::string>(
                ErrorCodes::BadValue,
                str::stream() << "invalid mode in " << filename << ": '" << line << "'");
        }

        // Check against acceptable values of opMode.
        if (opMode != "always" && opMode != "madvise" && opMode != "never") {
            return StatusWith<std::string>(
                ErrorCodes::BadValue,
                str::stream()
                    << "** WARNING: unrecognized transparent Huge Pages mode of operation in "
                    << filename
                    << ": '"
                    << opMode
                    << "''");
        }
    } catch (const boost::filesystem::filesystem_error& err) {
        return StatusWith<std::string>(ErrorCodes::UnknownError,
                                       str::stream() << "Failed to probe \"" << err.path1().string()
                                                     << "\": "
                                                     << err.code().message());
    }

    return StatusWith<std::string>(opMode);
}

void logMongodStartupWarnings(const StorageGlobalParams& storageParams,
                              const ServerGlobalParams& serverParams) {
    logCommonStartupWarnings(serverParams);

    bool warned = false;

    if (sizeof(int*) == 4) {
        log() << startupWarningsLog;
        log() << "** NOTE: This is a 32 bit MongoDB binary." << startupWarningsLog;
        log() << "**       32 bit builds are limited to less than 2GB of data "
              << "(or less with --journal)." << startupWarningsLog;
        if (!storageParams.dur) {
            log() << "**       Note that journaling defaults to off for 32 bit "
                  << "and is currently off." << startupWarningsLog;
        }
        log() << "**       See http://dochub.mongodb.org/core/32bit" << startupWarningsLog;
        warned = true;
    }

    if (!ProcessInfo::blockCheckSupported()) {
        log() << startupWarningsLog;
        log() << "** NOTE: your operating system version does not support the method that "
              << "MongoDB" << startupWarningsLog;
        log() << "**       uses to detect impending page faults." << startupWarningsLog;
        log() << "**       This may result in slower performance for certain use "
              << "cases" << startupWarningsLog;
        warned = true;
    }
#ifdef __linux__
    if (boost::filesystem::exists("/proc/vz") && !boost::filesystem::exists("/proc/bc")) {
        log() << startupWarningsLog;
        log() << "** WARNING: You are running in OpenVZ which can cause issues on versions "
              << "of RHEL older than RHEL6." << startupWarningsLog;
        warned = true;
    }

    bool hasMultipleNumaNodes = false;
    try {
        hasMultipleNumaNodes = boost::filesystem::exists("/sys/devices/system/node/node1");
    } catch (boost::filesystem::filesystem_error& e) {
        log() << startupWarningsLog;
        log() << "** WARNING: Cannot detect if NUMA interleaving is enabled. "
              << "Failed to probe \"" << e.path1().string() << "\": " << e.code().message()
              << startupWarningsLog;
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
                warning() << "failed to read from /proc/self/numa_maps: " << errnoWithDescription()
                          << startupWarningsLog;
                warned = true;
            } else {
                // skip over pointer
                std::string::size_type where = line.find(' ');
                if ((where == std::string::npos) || (++where == line.size())) {
                    log() << startupWarningsLog;
                    log() << "** WARNING: cannot parse numa_maps line: '" << line << "'"
                          << startupWarningsLog;
                    warned = true;
                }
                // if the text following the space doesn't begin with 'interleave', then
                // issue the warning.
                else if (line.find("interleave", where) != where) {
                    log() << startupWarningsLog;
                    log() << "** WARNING: You are running on a NUMA machine." << startupWarningsLog;
                    log() << "**          We suggest launching mongod like this to avoid "
                          << "performance problems:" << startupWarningsLog;
                    log() << "**              numactl --interleave=all mongod [other options]"
                          << startupWarningsLog;
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
            log() << startupWarningsLog;
            log() << "** WARNING: /proc/sys/vm/overcommit_memory is " << val << startupWarningsLog;
            log() << "**          Journaling works best with it set to 0 or 1"
                  << startupWarningsLog;
        }
    }

    if (boost::filesystem::exists("/proc/sys/vm/zone_reclaim_mode")) {
        std::fstream f("/proc/sys/vm/zone_reclaim_mode", ios_base::in);
        unsigned val;
        f >> val;

        if (val != 0) {
            log() << startupWarningsLog;
            log() << "** WARNING: /proc/sys/vm/zone_reclaim_mode is " << val << startupWarningsLog;
            log() << "**          We suggest setting it to 0" << startupWarningsLog;
            log() << "**          http://www.kernel.org/doc/Documentation/sysctl/vm.txt"
                  << startupWarningsLog;
        }
    }

    // Transparent Hugepages checks
    StatusWith<std::string> transparentHugePagesEnabledResult =
        StartupWarningsMongod::readTransparentHugePagesParameter("enabled");
    if (transparentHugePagesEnabledResult.isOK()) {
        if (transparentHugePagesEnabledResult.getValue() == "always") {
            log() << startupWarningsLog;
            log() << "** WARNING: " << kTransparentHugePagesDirectory << "/enabled is 'always'."
                  << startupWarningsLog;
            log() << "**        We suggest setting it to 'never'" << startupWarningsLog;
            warned = true;
        }
    } else if (transparentHugePagesEnabledResult.getStatus().code() !=
               ErrorCodes::NonExistentPath) {
        warning() << startupWarningsLog;
        warning() << transparentHugePagesEnabledResult.getStatus().reason() << startupWarningsLog;
        warned = true;
    }

    StatusWith<std::string> transparentHugePagesDefragResult =
        StartupWarningsMongod::readTransparentHugePagesParameter("defrag");
    if (transparentHugePagesDefragResult.isOK()) {
        if (transparentHugePagesDefragResult.getValue() == "always") {
            log() << startupWarningsLog;
            log() << "** WARNING: " << kTransparentHugePagesDirectory << "/defrag is 'always'."
                  << startupWarningsLog;
            log() << "**        We suggest setting it to 'never'" << startupWarningsLog;
            warned = true;
        }
    } else if (transparentHugePagesDefragResult.getStatus().code() != ErrorCodes::NonExistentPath) {
        warning() << startupWarningsLog;
        warning() << transparentHugePagesDefragResult.getStatus().reason() << startupWarningsLog;
        warned = true;
    }
#endif  // __linux__

#ifndef _WIN32
    // Check that # of files rlmit >= 1000
    const unsigned int minNumFiles = 1000;
    struct rlimit rlnofile;

    if (!getrlimit(RLIMIT_NOFILE, &rlnofile)) {
        if (rlnofile.rlim_cur < minNumFiles) {
            log() << startupWarningsLog;
            log() << "** WARNING: soft rlimits too low. Number of files is " << rlnofile.rlim_cur
                  << ", should be at least " << minNumFiles << startupWarningsLog;
        }
    } else {
        const auto errmsg = errnoWithDescription();
        log() << startupWarningsLog;
        log() << "** WARNING: getrlimit failed. " << errmsg << startupWarningsLog;
    }

// Solaris does not have RLIMIT_NPROC & RLIMIT_MEMLOCK, these are exposed via getrctl(2) instead
#ifndef __sun
    // Check # of processes >= # of files/2
    // Check we can lock at least 16 pages for the SecureAllocator
    const double filesToProcsRatio = 2.0;
    const unsigned int minLockedPages = 16;

    struct rlimit rlmemlock;
    struct rlimit rlnproc;

    if (!getrlimit(RLIMIT_NPROC, &rlnproc) && !getrlimit(RLIMIT_MEMLOCK, &rlmemlock)) {
        if ((rlmemlock.rlim_cur / ProcessInfo::getPageSize()) < minLockedPages) {
            log() << startupWarningsLog;
            log() << "** WARNING: soft rlimits too low. The locked memory size is "
                  << rlmemlock.rlim_cur << " bytes, it should be at least "
                  << minLockedPages * ProcessInfo::getPageSize() << " bytes" << startupWarningsLog;
        }

        if (false) {
            // juse to make things cleaner
        }
#ifdef __APPLE__
        else if (rlnproc.rlim_cur >= 709) {
            // os x doesn't make it easy to go higher
            // ERH thinks its ok not to add the warning in this case 7/3/2012
        }
#endif
        else if (rlnproc.rlim_cur < rlnofile.rlim_cur / filesToProcsRatio) {
            log() << startupWarningsLog;
            log() << "** WARNING: soft rlimits too low. rlimits set to " << rlnproc.rlim_cur
                  << " processes, " << rlnofile.rlim_cur
                  << " files. Number of processes should be at least "
                  << rlnofile.rlim_cur / filesToProcsRatio << " : " << 1 / filesToProcsRatio
                  << " times number of files." << startupWarningsLog;
        }
    } else {
        const auto errmsg = errnoWithDescription();
        log() << startupWarningsLog;
        log() << "** WARNING: getrlimit failed. " << errmsg << startupWarningsLog;
    }
#endif
#endif

#ifdef _WIN32
    ProcessInfo p;

    if (p.hasNumaEnabled()) {
        log() << startupWarningsLog;
        log() << "** WARNING: You are running on a NUMA machine." << startupWarningsLog;
        log() << "**          We suggest disabling NUMA in the machine BIOS " << startupWarningsLog;
        log() << "**          by enabling interleaving to avoid performance problems. "
              << startupWarningsLog;
        log() << "**          See your BIOS documentation for more information."
              << startupWarningsLog;
        warned = true;
    }
#endif  // #ifdef _WIN32

    if (storageParams.engine == "ephemeralForTest") {
        log() << startupWarningsLog;
        log() << "** NOTE: The ephemeralForTest storage engine is for testing only. "
              << startupWarningsLog;
        log() << "**       Do not use in production." << startupWarningsLog;
        warned = true;
    }

    if (warned) {
        log() << startupWarningsLog;
    }
}
}  // namespace mongo
