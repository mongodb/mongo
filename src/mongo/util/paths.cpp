/*    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/util/paths.h"

#include "mongo/platform/basic.h"
#include "mongo/util/log.h"

namespace mongo {

    void flushMyDirectory(const boost::filesystem::path& file) {
#ifdef __linux__ // this isn't needed elsewhere
        static bool _warnedAboutFilesystem = false;
        // if called without a fully qualified path it asserts; that makes mongoperf fail.
        // so make a warning. need a better solution longer term.
        // massert(13652, str::stream() << "Couldn't find parent dir for file: " << file.string(),);
        if (!file.has_branch_path()) {
            log() << "warning flushMyDirectory couldn't find parent dir for file: "
                  << file.string();
            return;
        }


        boost::filesystem::path dir = file.branch_path(); // parent_path in new boosts

        LOG(1) << "flushing directory " << dir.string();

        int fd = ::open(dir.string().c_str(), O_RDONLY); // DO NOT THROW OR ASSERT BEFORE CLOSING
        massert(13650, str::stream() << "Couldn't open directory '" << dir.string()
                                     << "' for flushing: " << errnoWithDescription(),
                fd >= 0);
        if (fsync(fd) != 0) {
            int e = errno;
            if (e == EINVAL) { // indicates filesystem does not support synchronization
                if (!_warnedAboutFilesystem) {
                    log() << "\tWARNING: This file system is not supported. For further information"
                          << " see:"
                          << startupWarningsLog;
                    log() << "\t\t\thttp://dochub.mongodb.org/core/unsupported-filesystems"
                          << startupWarningsLog;
                    log() << "\t\tPlease notify MongoDB, Inc. if an unlisted filesystem generated "
                          << "this warning." << startupWarningsLog;
                    _warnedAboutFilesystem = true;
                }
            }
            else {
                close(fd);
                massert(13651, str::stream() << "Couldn't fsync directory '" << dir.string()
                                             << "': " << errnoWithDescription(e),
                        false);
            }
        }
        close(fd);
#endif
    }
}
