/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
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
