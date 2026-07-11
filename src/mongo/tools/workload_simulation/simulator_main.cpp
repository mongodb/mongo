// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/initializer.h"
#include "mongo/db/server_options_base.h"
#include "mongo/db/server_options_helpers.h"
#include "mongo/logv2/log_debug.h"
#include "mongo/tools/workload_simulation/simulation.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/text.h"

namespace mongo::workload_simulation {
namespace {

/**
 * Runs registered and selected simulator workloads.
 */
ExitCode simulatorMain(int argc, char** argv) {
    runGlobalInitializersOrDie(std::vector<std::string>(argv, argv + argc));

    SimulationRegistry::get().runSelected();

    return ExitCode::clean;
}

}  // namespace
}  // namespace mongo::workload_simulation

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables simulatorMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[]) {
    mongo::quickExit(mongo::workload_simulation::simulatorMain(
        argc, mongo::WindowsCommandLine(argc, argvW).argv()));
}
#else
int main(int argc, char* argv[]) {
    mongo::quickExit(mongo::workload_simulation::simulatorMain(argc, argv));
}
#endif
