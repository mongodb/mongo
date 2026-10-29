// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/quick_exit.h"

#include <iostream>
#include <string>
#include <vector>

namespace mongo {
namespace optionenvironment {
namespace {

MONGO_STARTUP_OPTIONS_PARSE(StartupOptions)(InitializerContext* context) {
    OptionsParser parser;
    Status ret = parser.run(startupOptions, context->args(), &startupOptionsParsed);
    if (!ret.isOK()) {
        std::cerr << ret.reason() << std::endl;
        // TODO: Figure out if there's a use case for this help message ever being different
        std::cerr << "try '" << context->args()[0] << " --help' for more information" << std::endl;
        quickExit(ExitCode::badOptions);
    }
}

MONGO_INITIALIZER_GENERAL(OutputConfig,
                          ("EndStartupOptionValidation"),
                          ("BeginStartupOptionStorage"))
(InitializerContext*) {
    if (startupOptionsParsed.count("outputConfig")) {
        bool output = false;
        uassertStatusOK(startupOptionsParsed.get(Key("outputConfig"), &output));
        if (output) {
            std::cout << startupOptionsParsed.toYAML() << std::endl;
            quickExit(ExitCode::clean);
        }
    }
}

}  // namespace
}  // namespace optionenvironment
}  // namespace mongo
