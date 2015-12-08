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

#ifndef UTIL_VERSION_HEADER
#define UTIL_VERSION_HEADER

#include <string>

#include "mongo/bson/bsonobj.h"

namespace mongo {
struct BSONArray;
class BSONObjBuilder;

// mongo version
extern const char versionString[];
extern const int versionNumber;
std::string mongodVersion();

// mongo git version
const char* gitVersion();
const char* distName();
std::vector<std::string> compiledModules();

// Checks whether another version is the same major version as us
bool isSameMajorVersion(const char* version);

// Get/print the version of OpenSSL that's used at runtime
const std::string openSSLVersion(const std::string& prefix = "", const std::string& suffix = "");

// Append build info data to a BSONObjBuilder
void appendBuildInfo(BSONObjBuilder& result);

void printTargetMinOS();
void printBuildInfo();
void show_warnings();

extern const int kMongoVersionMajor;
extern const int kMongoVersionMinor;
extern const int kMongoVersionPatch;
extern const int kMongoVersionExtra;
extern const char kMongoVersionExtraStr[];

}  // namespace mongo

#endif  // UTIL_VERSION_HEADER
