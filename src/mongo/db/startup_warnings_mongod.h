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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"

namespace mongo {

struct StorageGlobalParams;
struct ServerGlobalParams;

class StartupWarningsMongod {
private:
    StartupWarningsMongod();

public:
    /**
     * Reads Transparent HugePages kernel parameter in sysfs directory.
     * Linux only.
     */
    static StatusWith<std::string> readTransparentHugePagesParameter(const std::string& parameter);

    /**
     * For testing only.
     * Supports alternate directory for transparent huge pages files.
     */
    static StatusWith<std::string> readTransparentHugePagesParameter(const std::string& parameter,
                                                                     const std::string& directory);
};

// Checks various startup conditions and logs any necessary warnings that
// are specific to the mongod process.
void logMongodStartupWarnings(const StorageGlobalParams& storageParams,
                              const ServerGlobalParams& serverParams);
}  // namespace mongo
