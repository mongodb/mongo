// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/tools/mongobridge_tool/mongobridge_options.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/options_parser/value.h"
#include "mongo/util/quick_exit.h"

#include <iostream>
#include <string>
#include <vector>

namespace mongo {

MONGO_STARTUP_OPTIONS_VALIDATE(MongoBridgeOptions)(InitializerContext* context) {
    if (!handlePreValidationMongoBridgeOptions(moe::startupOptionsParsed)) {
        quickExit(ExitCode::clean);
    }
    uassertStatusOK(moe::startupOptionsParsed.validate());
}

MONGO_STARTUP_OPTIONS_STORE(MongoBridgeOptions)(InitializerContext* context) {
    Status ret = storeMongoBridgeOptions(moe::startupOptionsParsed, context->args());
    if (!ret.isOK()) {
        std::cerr << ret.toString() << std::endl;
        std::cerr << "try '" << context->args()[0] << " --help' for more information" << std::endl;
        quickExit(ExitCode::badOptions);
    }

    if (moe::startupOptionsParsed.count("net.compression.compressors")) {
        const auto ret = storeMessageCompressionOptions(
            moe::startupOptionsParsed["net.compression.compressors"].as<std::string>());
        if (!ret.isOK()) {
            std::cerr << ret.toString() << std::endl;
            quickExit(ExitCode::badOptions);
        }
    }
}
}  // namespace mongo
