// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/logv2/log_debug.h"
#include "mongo/tools/workload_simulation/simulator_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/quick_exit.h"

#include <iostream>
#include <string>
#include <vector>

namespace mongo::workload_simulation {
MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(SimulatorOptions)(InitializerContext* context) {
    uassertStatusOK(addSimulatorOptions(&optionenvironment::startupOptions));
}

MONGO_STARTUP_OPTIONS_VALIDATE(SimulatorOptions)(InitializerContext* context) {
    if (!handlePreValidationSimulatorOptions(optionenvironment::startupOptionsParsed,
                                             context->args())) {
        quickExit(ExitCode::clean);
    }
    uassertStatusOK(optionenvironment::startupOptionsParsed.validate());
}

MONGO_STARTUP_OPTIONS_STORE(SimulatorOptions)(InitializerContext* context) {
    Status ret = storeSimulatorOptions(optionenvironment::startupOptionsParsed, context->args());
    if (!ret.isOK()) {
        std::cerr << ret.toString() << std::endl;
        std::cerr << "try '" << context->args()[0] << " --help' for more information" << std::endl;
        quickExit(ExitCode::badOptions);
    }
}

MONGO_INITIALIZER_GENERAL(CoreOptions_Store, (), ())
(InitializerContext* context) {}
}  // namespace mongo::workload_simulation
