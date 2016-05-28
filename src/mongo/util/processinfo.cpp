// processinfo.cpp

/*    Copyright 2009 10gen Inc.
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

#include "mongo/base/init.h"
#include "mongo/util/processinfo.h"

#include <fstream>
#include <iostream>

#include "mongo/util/log.h"

using namespace std;

namespace mongo {

class PidFileWiper {
public:
    ~PidFileWiper() {
        if (path.empty()) {
            return;
        }

        ofstream out(path.c_str(), ios_base::out);
        out.close();
    }

    bool write(const string& p) {
        path = p;
        ofstream out(path.c_str(), ios_base::out);
        out << ProcessId::getCurrent() << endl;
        if (!out.good()) {
            auto errAndStr = errnoAndDescription();
            if (errAndStr.first == 0) {
                log() << "ERROR: Cannot write pid file to " << path
                      << ": Unable to determine OS error";
            } else {
                log() << "ERROR: Cannot write pid file to " << path << ": " << errAndStr.second;
            }
        }
        return out.good();
    }

    string path;
} pidFileWiper;

bool writePidFile(const string& path) {
    return pidFileWiper.write(path);
}

ProcessInfo::SystemInfo* ProcessInfo::systemInfo = NULL;

void ProcessInfo::initializeSystemInfo() {
    if (systemInfo == NULL) {
        systemInfo = new SystemInfo();
    }
}

/**
 * We need this get the system page size for the secure allocator, which the enterprise modules need
 * for storage for command line parameters.
 */
MONGO_INITIALIZER_GENERAL(SystemInfo, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS)
(InitializerContext* context) {
    ProcessInfo::initializeSystemInfo();
    return Status::OK();
}
}
