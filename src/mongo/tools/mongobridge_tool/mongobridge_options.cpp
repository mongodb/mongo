// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/tools/mongobridge_tool/mongobridge_options.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_component_settings.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/random.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/options_parser/value.h"

#include <algorithm>
#include <iostream>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kBridge


namespace mongo {

MongoBridgeGlobalParams mongoBridgeGlobalParams;

void printMongoBridgeHelp(std::ostream* out) {
    *out << "Usage: mongobridge --port <port> --dest <dest> [ --seed <seed> ] [ --verbose <vvv> ]"
            " [ --help ]"
         << std::endl;
    *out << moe::startupOptions.helpString();
    *out << std::flush;
}

bool handlePreValidationMongoBridgeOptions(const moe::Environment& params) {
    if (params.count("help")) {
        printMongoBridgeHelp(&std::cout);
        return false;
    }
    return true;
}

Status storeMongoBridgeOptions(const moe::Environment& params,
                               const std::vector<std::string>& args) {
    if (!params.count("port")) {
        return {ErrorCodes::BadValue, "Missing required option: --port"};
    }

    if (!params.count("dest")) {
        return {ErrorCodes::BadValue, "Missing required option: --dest"};
    }

    if (!params.count("seed")) {
        mongoBridgeGlobalParams.seed = SecureRandom().nextInt64();
    } else {
        mongoBridgeGlobalParams.seed = static_cast<int64_t>(params["seed"].as<long>());
    }

    if (params.count("verbose")) {
        std::string verbosity = params["verbose"].as<std::string>();
        if (std::any_of(verbosity.cbegin(), verbosity.cend(), [](char ch) { return ch != 'v'; })) {
            return {ErrorCodes::BadValue,
                    "The string for the --verbose option cannot contain characters other than 'v'"};
        }

        logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
            mongo::logv2::LogComponent::kDefault, logv2::LogSeverity::Debug(verbosity.length()));
    }

    return Status::OK();
}

}  // namespace mongo
