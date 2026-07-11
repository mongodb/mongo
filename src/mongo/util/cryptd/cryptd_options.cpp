// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/util/cryptd/cryptd_options.h"

#include "mongo/base/status.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_options_base.h"
#include "mongo/db/server_options_server_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/version.h"

#include <iostream>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

MongoCryptDGlobalParams mongoCryptDGlobalParams;

Status addMongoCryptDOptions(moe::OptionSection* options) {
    return addGeneralServerOptions(options);
}

void printMongoCryptDHelp(std::ostream* out) {
    *out << moe::startupOptions.helpString();
    *out << std::flush;
}

bool handlePreValidationMongoCryptDOptions(const moe::Environment& params) {
    if (params.count("help") && params["help"].as<bool>() == true) {
        printMongoCryptDHelp(&std::cout);
        return false;
    }

    if (params.count("version") && params["version"].as<bool>() == true) {
        auto&& vii = VersionInfoInterface::instance();
        std::cout << mongocryptVersion(vii) << std::endl;
        vii.logBuildInfo(&std::cout);
        return false;
    }

    return true;
}


Status validateMongoCryptDOptions(const moe::Environment& params) {
    return validateServerOptions(params);
}

Status canonicalizeMongoCryptDOptions(moe::Environment* params) {
    return canonicalizeServerOptions(params);
}

Status storeMongoCryptDOptions(const moe::Environment& params,
                               const std::vector<std::string>& args) {
    Status ret = storeServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

    if (!params.count("net.port")) {
        mongoCryptDGlobalParams.port = ServerGlobalParams::CryptDServerPort;
    } else {
        mongoCryptDGlobalParams.port = params["net.port"].as<int>();
    }

    mongoCryptDGlobalParams.idleShutdownTimeout =
        Seconds(params["processManagement.idleShutdownTimeoutSecs"].as<int>());

    // When the user passes processManagement.pidFilePath, it is handled in other
    // option parsing code. We just set a default here if it is not set.
    if (!params.count("processManagement.pidFilePath")) {
        serverGlobalParams.pidFile = "mongocryptd.pid";
    }

    return Status::OK();
}

}  // namespace mongo
