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
        auto&& vii = VersionInfoInterface::instance();
        if ((vii.minorVersion() % 2) != 0) {
            log() << startupWarningsLog;
            log() << "** NOTE: This is a development version (" << vii.version() << ") of MongoDB."
                  << startupWarningsLog;
            log() << "**       Not recommended for production." << startupWarningsLog;
            warned = true;
        }
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

    if (serverParams.bind_ip.empty()) {
        log() << startupWarningsLog;
        log() << "** WARNING: This server is bound to localhost." << startupWarningsLog;
        log() << "**          Remote systems will be unable to connect to this server. "
              << startupWarningsLog;
        log() << "**          Start the server with --bind_ip <address> to specify which IP "
              << startupWarningsLog;
        log() << "**          addresses it should serve responses from, or with --bind_ip_all to"
              << startupWarningsLog;
        log() << "**          bind to all interfaces. If this behavior is desired, start the"
              << startupWarningsLog;
        log() << "**          server with --bind_ip 127.0.0.1 to disable this warning."
              << startupWarningsLog;
        warned = true;
    }


    if (warned) {
        log() << startupWarningsLog;
    }
}
}  // namespace mongo
