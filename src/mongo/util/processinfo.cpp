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

#define MONGO_PCH_WHITELISTED
#include "mongo/platform/basic.h"
#include "mongo/pch.h"
#undef MONGO_PCH_WHITELISTED

#include "mongo/base/init.h"
#include "mongo/util/processinfo.h"

#include <iostream>
#include <fstream>

#include "mongo/util/log.h"

using namespace std;

namespace mongo {

    class PidFileWiper {
    public:
        ~PidFileWiper() {
            if (path.empty()) {
                return;
            }

            ofstream out( path.c_str() , ios_base::out );
            out.close();
        }

        bool write( const string& p ) {
            path = p;
            ofstream out( path.c_str() , ios_base::out );
            out << ProcessId::getCurrent() << endl;
            return out.good();
        }

        string path;
    } pidFileWiper;

    bool writePidFile( const string& path ) {
        bool e = pidFileWiper.write( path );
        if (!e) {
            log() << "ERROR: Cannot write pid file to " << path
                  << ": "<< strerror(errno);
        }
        return e;
    }

    ProcessInfo::SystemInfo* ProcessInfo::systemInfo = NULL;

    void ProcessInfo::initializeSystemInfo() {
        if (systemInfo == NULL) {
            systemInfo = new SystemInfo();
        }
    }

    MONGO_INITIALIZER(SystemInfo)(InitializerContext* context) {
        ProcessInfo::initializeSystemInfo();
        return Status::OK();
    }

}
