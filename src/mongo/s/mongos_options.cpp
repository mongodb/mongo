/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/mongos_options.h"

#include <iostream>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/builder.h"
#include "mongo/config.h"
#include "mongo/db/server_options_base.h"
#include "mongo/db/server_options_server_helpers.h"
#include "mongo/s/version_mongos.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/str.h"

namespace mongo {

MongosGlobalParams mongosGlobalParams;

void printMongosHelp(const moe::OptionSection& options) {
    std::cout << options.helpString() << std::endl;
};

bool handlePreValidationMongosOptions(const moe::Environment& params,
                                      const std::vector<std::string>& args) {
    if (params.count("help") && params["help"].as<bool>() == true) {
        printMongosHelp(moe::startupOptions);
        return false;
    }
    if (params.count("version") && params["version"].as<bool>() == true) {
        printShardingVersionInfo(true);
        return false;
    }
    if (params.count("test") && params["test"].as<bool>() == true) {
        setMinimumLoggedSeverity(::mongo::logger::LogSeverity::Debug(5));
        return false;
    }

    return true;
}

Status validateMongosOptions(const moe::Environment& params) {
    Status ret = validateServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

    return Status::OK();
}

Status canonicalizeMongosOptions(moe::Environment* params) {
    Status ret = canonicalizeServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

    // "security.javascriptEnabled" comes from the config file, so override it if "noscripting"
    // is set since that comes from the command line.
    if (params->count("noscripting")) {
        auto status = params->set("security.javascriptEnabled",
                                  moe::Value(!(*params)["noscripting"].as<bool>()));
        if (!status.isOK()) {
            return status;
        }

        status = params->remove("noscripting");
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status storeMongosOptions(const moe::Environment& params) {
    Status ret = storeServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

    if (params.count("net.port")) {
        int port = params["net.port"].as<int>();
        if (port <= 0 || port > 65535) {
            return Status(ErrorCodes::BadValue, "error: port number must be between 1 and 65535");
        }
    }

    if (params.count("security.javascriptEnabled")) {
        mongosGlobalParams.scriptingEnabled = params["security.javascriptEnabled"].as<bool>();
    }

    if (!params.count("sharding.configDB")) {
        return Status(ErrorCodes::BadValue, "error: no args for --configdb");
    }

    std::string configdbString = params["sharding.configDB"].as<std::string>();

    auto configdbConnectionString = ConnectionString::parse(configdbString);
    if (!configdbConnectionString.isOK()) {
        return configdbConnectionString.getStatus();
    }

    if (configdbConnectionString.getValue().type() != ConnectionString::SET) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "configdb supports only replica set connection string");
    }

    std::vector<HostAndPort> seedServers;
    bool resolvedSomeSeedSever = false;
    for (const auto& host : configdbConnectionString.getValue().getServers()) {
        seedServers.push_back(host);
        if (!seedServers.back().hasPort()) {
            seedServers.back() = HostAndPort{host.host(), ServerGlobalParams::ConfigServerPort};
        }
        if (!hostbyname(seedServers.back().host().c_str()).empty()) {
            resolvedSomeSeedSever = true;
        }
    }
    if (!resolvedSomeSeedSever) {
        if (!hostbyname(configdbConnectionString.getValue().getSetName().c_str()).empty()) {
            warning() << "The replica set name \""
                      << str::escape(configdbConnectionString.getValue().getSetName())
                      << "\" resolves as a host name, but none of the servers in the seed list do. "
                         "Did you reverse the replica set name and the seed list in "
                      << str::escape(configdbConnectionString.getValue().toString()) << "?";
        }
    }

    mongosGlobalParams.configdbs =
        ConnectionString{configdbConnectionString.getValue().type(),
                         seedServers,
                         configdbConnectionString.getValue().getSetName()};

    if (mongosGlobalParams.configdbs.getServers().size() < 3) {
        warning() << "Running a sharded cluster with fewer than 3 config servers should only be "
                     "done for testing purposes and is not recommended for production.";
    }

    return Status::OK();
}

}  // namespace mongo
