/**
*    Copyright (C) 2012 10gen Inc.
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
*/

#ifndef UTIL_VERSION_HEADER
#define UTIL_VERSION_HEADER

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
    struct BSONArray;

    // mongo version
    extern const char versionString[];
    extern const BSONArray versionArray;
    std::string mongodVersion();

    // Convert a version string into a numeric array
    BSONArray toVersionArray(const char* version);
    
    // Checks whether another version is the same major version as us
    bool isSameMajorVersion(const char* version);

    void appendBuildInfo(BSONObjBuilder& result);

    const char * gitVersion();
    const char * compiledJSEngine();
    const char * allocator();
    const char * loaderFlags();
    const char * compilerFlags();

    void printGitVersion();

    const std::string openSSLVersion(const std::string &prefix = "", const std::string &suffix = "");
    void printOpenSSLVersion();

    std::string sysInfo();
    void printSysInfo();
    void printAllocator();

    void show_warnings();

}  // namespace mongo

#endif  // UTIL_VERSION_HEADER
