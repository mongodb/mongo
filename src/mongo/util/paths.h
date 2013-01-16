// @file paths.h
// file paths and directory handling

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

#pragma once

#include "mongoutils/str.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <boost/filesystem/path.hpp>

namespace mongo {
    
    using namespace mongoutils;

    extern string dbpath;

    /** this is very much like a boost::path.  however, we define a new type to get some type
        checking.  if you want to say 'my param MUST be a relative path", use this.
    */
    struct RelativePath {
        string _p;

        bool empty() const { return _p.empty(); }

        static RelativePath fromRelativePath(const std::string& f) {
            RelativePath rp;
            rp._p = f;
            return rp;
        }
        
        /** from a full path */
        static RelativePath fromFullPath(boost::filesystem::path f) {
            boost::filesystem::path dbp(dbpath); // normalizes / and backslash
            string fullpath = f.string();
            string relative = str::after(fullpath, dbp.string());
            if( relative.empty() ) {
                log() << "warning file is not under db path? " << fullpath << ' ' << dbp.string() << endl;
                RelativePath rp;
                rp._p = fullpath;
                return rp;
            }
            /*uassert(13600,
                    str::stream() << "file path is not under the db path? " << fullpath << ' ' << dbpath,
                    relative != fullpath);*/
            if( str::startsWith(relative, "/") || str::startsWith(relative, "\\") ) {
                relative.erase(0, 1);
            }
            RelativePath rp;
            rp._p = relative;
            return rp;
        }

        string toString() const { return _p; }

        bool operator!=(const RelativePath& r) const { return _p != r._p; }
        bool operator==(const RelativePath& r) const { return _p == r._p; }
        bool operator<(const RelativePath& r) const { return _p < r._p; }

        string asFullPath() const {
            boost::filesystem::path x(dbpath);
            x /= _p;
            return x.string();
        }

    };

    inline dev_t getPartition(const string& path){
        struct stat stats;

        if (stat(path.c_str(), &stats) != 0){
            uasserted(13646, str::stream() << "stat() failed for file: " << path << " " << errnoWithDescription());
        }

        return stats.st_dev;
    }
    
    inline bool onSamePartition(const string& path1, const string& path2){
        dev_t dev1 = getPartition(path1);
        dev_t dev2 = getPartition(path2);

        return dev1 == dev2;
    }

    inline void flushMyDirectory(const boost::filesystem::path& file){
#ifdef __linux__ // this isn't needed elsewhere
        // if called without a fully qualified path it asserts; that makes mongoperf fail. so make a warning. need a better solution longer term.
        // massert(13652, str::stream() << "Couldn't find parent dir for file: " << file.string(), );
        if( !file.has_branch_path() ) {
            log() << "warning flushMYDirectory couldn't find parent dir for file: " << file.string() << endl;
            return;
        }


        boost::filesystem::path dir = file.branch_path(); // parent_path in new boosts

        LOG(1) << "flushing directory " << dir.string() << endl;

        int fd = ::open(dir.string().c_str(), O_RDONLY); // DO NOT THROW OR ASSERT BEFORE CLOSING
        massert(13650, str::stream() << "Couldn't open directory '" << dir.string() << "' for flushing: " << errnoWithDescription(), fd >= 0);
        if (fsync(fd) != 0){
            int e = errno;
            close(fd);
            massert(13651, str::stream() << "Couldn't fsync directory '" << dir.string() << "': " << errnoWithDescription(e), false);
        }
        close(fd);
#endif
    }

    boost::filesystem::path ensureParentDirCreated(const boost::filesystem::path& p);

}
