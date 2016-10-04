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

/**
 * The ntservice namespace provides minimal support for running mongo servers as NT services.
 *
 * TODO: ntservice should only provide implementation for a more general server process
 * startup/shutdown/management interface.
 */

#pragma once

#ifdef _WIN32

#include <string>
#include <vector>

#include "mongo/platform/compiler.h"
#include "mongo/util/exit_code.h"

namespace mongo {

namespace optionenvironment {
class OptionSection;
class Environment;
}  // namespace optionenvironment

namespace moe = mongo::optionenvironment;

namespace ntservice {
struct NtServiceDefaultStrings {
    const wchar_t* serviceName;
    const wchar_t* displayName;
    const wchar_t* serviceDescription;
};

typedef ExitCode (*ServiceCallback)(void);

/**
 * Configure the service.
 *
 * Also performs service installation and removal.
 *
 * This function calls _exit() with an error if bad parameters are passed in.  If
 * the parameters specify that the service should be installed, removed, etc, performs that
 * operation and exits.
 *
 * If this function returns to the caller, the caller should either call startService, or run
 * the service as a regular process, depending on the return value of shouldStartService().
 */
void configureService(ServiceCallback serviceCallback,
                      const moe::Environment& params,
                      const NtServiceDefaultStrings& defaultStrings,
                      const std::vector<std::string>& disallowedOptions,
                      const std::vector<std::string>& argv);

bool shouldStartService();

/**
 * Construct an argv array that Windows should use to start mongod/mongos as a service
 * if mongo was started with "inputArgv", which is assumed to be an argument vector that
 * dictates that Windows should install mongo as a service.
 *
 * The result is suitable for passing to mongo::constructUtf8WindowsCommandLine() to construct
 * a properly quoted command line string.
 */
std::vector<std::string> constructServiceArgv(const std::vector<std::string>& inputArgv);

/**
 * Start the service.  Never returns.
 */
MONGO_COMPILER_NORETURN void startService();

bool reportStatus(DWORD reportState, DWORD waitHint = 0, DWORD exitCode = 0);

}  // namespace ntservice
}  // namespace mongo

#endif  // defined(_WIN32)
