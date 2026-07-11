// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * The ntservice namespace provides minimal support for running mongo servers as NT services.
 *
 * TODO: ntservice should only provide implementation for a more general server process
 * startup/shutdown/management interface.
 */

#pragma once

#ifdef _WIN32

#include "mongo/platform/compiler.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

namespace mongo {

namespace optionenvironment {
class OptionSection;
class Environment;
}  // namespace optionenvironment

namespace moe = mongo::optionenvironment;

namespace [[MONGO_MOD_PUBLIC]] ntservice {
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
