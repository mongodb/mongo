// @file paths.h
// file paths and directory handling

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

#pragma once

#include <boost/filesystem/path.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "mongo/util/mongoutils/str.h"

#include "mongo/db/storage/storage_options.h"

namespace mongo {

using namespace mongoutils;

/** this is very much like a boost::path.  however, we define a new type to get some type
    checking.  if you want to say 'my param MUST be a relative path", use this.
*/
struct RelativePath {
    std::string _p;

    bool empty() const {
        return _p.empty();
    }

    static RelativePath fromRelativePath(const std::string& f) {
        RelativePath rp;
        rp._p = f;
        return rp;
    }

    /**
     * Returns path relative to 'dbpath' from a full path 'f'.
     */
    static RelativePath fromFullPath(boost::filesystem::path dbpath, boost::filesystem::path f);

    std::string toString() const {
        return _p;
    }

    bool operator!=(const RelativePath& r) const {
        return _p != r._p;
    }
    bool operator==(const RelativePath& r) const {
        return _p == r._p;
    }
    bool operator<(const RelativePath& r) const {
        return _p < r._p;
    }

    std::string asFullPath() const {
        boost::filesystem::path x(storageGlobalParams.dbpath);
        x /= _p;
        return x.string();
    }
};

dev_t getPartition(const std::string& path);

inline bool onSamePartition(const std::string& path1, const std::string& path2) {
    dev_t dev1 = getPartition(path1);
    dev_t dev2 = getPartition(path2);

    return dev1 == dev2;
}

void flushMyDirectory(const boost::filesystem::path& file);

boost::filesystem::path ensureParentDirCreated(const boost::filesystem::path& p);
}
