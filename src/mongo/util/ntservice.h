/*    Copyright 2009 10gen Inc.
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

namespace mongo {

    namespace optionenvironment {
        class OptionSection;
        class Environment;
    } // namespace optionenvironment

    namespace moe = mongo::optionenvironment;

namespace ntservice {
    struct NtServiceDefaultStrings {
        const wchar_t* serviceName;
        const wchar_t* displayName;
        const wchar_t* serviceDescription;
    };

    typedef void (*ServiceCallback)(void);

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
    void configureService(
            ServiceCallback serviceCallback,
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

    bool reportStatus(DWORD reportState, DWORD waitHint = 0);

}  // namespace ntservice
}  // namespace mongo

#endif  // defined(_WIN32)
