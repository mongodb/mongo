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

#include <boost/filesystem/path.hpp>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

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
                log() << "warning file is not under db path? " << fullpath << ' ' << dbp.string();
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

    void flushMyDirectory(const boost::filesystem::path& file);

    boost::filesystem::path ensureParentDirCreated(const boost::filesystem::path& p);

}
