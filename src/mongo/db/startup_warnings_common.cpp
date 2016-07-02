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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/db/startup_warnings_common.h"

#include <boost/filesystem/operations.hpp>
#include <fstream>

#include "mongo/db/server_options.h"
#include "mongo/util/log.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/version.h"

namespace mongo {

//
// system warnings
//
void logCommonStartupWarnings(const ServerGlobalParams& serverParams) {
    // each message adds a leading and a trailing newline

    bool warned = false;
    {
        const char* foo = strchr(versionString, '.') + 1;
        int bar = atoi(foo);
        if ((2 * (bar / 2)) != bar) {
            log() << startupWarningsLog;
            log() << "** NOTE: This is a development version (" << versionString << ") of MongoDB."
                  << startupWarningsLog;
            log() << "**       Not recommended for production." << startupWarningsLog;
            warned = true;
        }
    }

    if (serverParams.authState == ServerGlobalParams::AuthState::kEnabled &&
        (serverParams.rest || serverParams.isHttpInterfaceEnabled || serverParams.jsonp)) {
        log() << startupWarningsLog;
        log()
            << "** WARNING: The server is started with the web server interface and access control."
            << startupWarningsLog;
        log() << "**          The web interfaces (rest, httpinterface and/or jsonp) are insecure "
              << startupWarningsLog;
        log() << "**          and should be disabled unless required for backward compatibility."
              << startupWarningsLog;
        warned = true;
    }

    if (serverParams.authState == ServerGlobalParams::AuthState::kUndefined) {
        log() << startupWarningsLog;
        log() << "** WARNING: Access control is not enabled for the database."
              << startupWarningsLog;
        log() << "**          Read and write access to data and configuration is "
                 "unrestricted."
              << startupWarningsLog;
        warned = true;
    }

    const bool is32bit = sizeof(int*) == 4;
    if (is32bit) {
        log() << startupWarningsLog;
        log() << "** WARNING: This 32-bit MongoDB binary is deprecated" << startupWarningsLog;
        warned = true;
    }

    /*
    * We did not add the message to startupWarningsLog as the user can not
    * specify a sslCAFile parameter from the shell
    */
    if (sslGlobalParams.sslMode.load() != SSLParams::SSLMode_disabled &&
        sslGlobalParams.sslCAFile.empty()) {
        log() << "";
        log() << "** WARNING: No SSL certificate validation can be performed since"
                 " no CA file has been provided";

        log() << "**          Please specify an sslCAFile parameter.";
    }

#if defined(_WIN32) && !defined(_WIN64)
    // Warn user that they are running a 32-bit app on 64-bit Windows
    BOOL wow64Process;
    BOOL retWow64 = IsWow64Process(GetCurrentProcess(), &wow64Process);
    if (retWow64 && wow64Process) {
        log() << "** NOTE: This is a 32-bit MongoDB binary running on a 64-bit operating"
              << startupWarningsLog;
        log() << "**      system. Switch to a 64-bit build of MongoDB to" << startupWarningsLog;
        log() << "**      support larger databases." << startupWarningsLog;
        warned = true;
    }
#endif

#if !defined(_WIN32)
    if (getuid() == 0) {
        log() << "** WARNING: You are running this process as the root user, "
              << "which is not recommended." << startupWarningsLog;
        warned = true;
    }
#endif

    if (warned) {
        log() << startupWarningsLog;
    }
}
}  // namespace mongo
