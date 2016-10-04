/*    Copyright 2010 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/paths.h"

#include "mongo/util/log.h"

namespace mongo {

/** from a full path */
RelativePath RelativePath::fromFullPath(boost::filesystem::path dbp, boost::filesystem::path f) {
    // filesystem::path normalizes / and backslash
    std::string fullpath = f.string();
    std::string relative = str::after(fullpath, dbp.string());
    if (relative.empty()) {
        log() << "warning file is not under db path? " << fullpath << ' ' << dbp.string();
        RelativePath rp;
        rp._p = fullpath;
        return rp;
    }
    if (str::startsWith(relative, "/") || str::startsWith(relative, "\\")) {
        relative.erase(0, 1);
    }
    RelativePath rp;
    rp._p = relative;
    return rp;
}

dev_t getPartition(const std::string& path) {
    struct stat stats;

    if (stat(path.c_str(), &stats) != 0) {
        uasserted(13646,
                  str::stream() << "stat() failed for file: " << path << " "
                                << errnoWithDescription());
    }

    return stats.st_dev;
}

void flushMyDirectory(const boost::filesystem::path& file) {
#ifdef __linux__  // this isn't needed elsewhere
    static bool _warnedAboutFilesystem = false;
    // if called without a fully qualified path it asserts; that makes mongoperf fail.
    // so make a warning. need a better solution longer term.
    // massert(13652, str::stream() << "Couldn't find parent dir for file: " << file.string(),);
    if (!file.has_branch_path()) {
        log() << "warning flushMyDirectory couldn't find parent dir for file: " << file.string();
        return;
    }


    boost::filesystem::path dir = file.branch_path();  // parent_path in new boosts

    LOG(1) << "flushing directory " << dir.string();

    int fd = ::open(dir.string().c_str(), O_RDONLY);  // DO NOT THROW OR ASSERT BEFORE CLOSING
    massert(13650,
            str::stream() << "Couldn't open directory '" << dir.string() << "' for flushing: "
                          << errnoWithDescription(),
            fd >= 0);
    if (fsync(fd) != 0) {
        int e = errno;
        if (e == EINVAL) {  // indicates filesystem does not support synchronization
            if (!_warnedAboutFilesystem) {
                log() << "\tWARNING: This file system is not supported. For further information"
                      << " see:" << startupWarningsLog;
                log() << "\t\t\thttp://dochub.mongodb.org/core/unsupported-filesystems"
                      << startupWarningsLog;
                log() << "\t\tPlease notify MongoDB, Inc. if an unlisted filesystem generated "
                      << "this warning." << startupWarningsLog;
                _warnedAboutFilesystem = true;
            }
        } else {
            close(fd);
            massert(13651,
                    str::stream() << "Couldn't fsync directory '" << dir.string() << "': "
                                  << errnoWithDescription(e),
                    false);
        }
    }
    close(fd);
#endif
}
}
