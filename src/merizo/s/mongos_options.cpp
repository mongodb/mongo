/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MERIZO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kSharding

#include "merizo/platform/basic.h"

#include "merizo/s/merizos_options.h"

#include <iostream>
#include <string>
#include <vector>

#include "merizo/base/status.h"
#include "merizo/base/status_with.h"
#include "merizo/bson/util/builder.h"
#include "merizo/config.h"
#include "merizo/db/server_options_base.h"
#include "merizo/db/server_options_server_helpers.h"
#include "merizo/s/version_merizos.h"
#include "merizo/util/log.h"
#include "merizo/util/merizoutils/str.h"
#include "merizo/util/net/socket_utils.h"
#include "merizo/util/options_parser/startup_options.h"
#include "merizo/util/startup_test.h"
#include "merizo/util/stringutils.h"

namespace merizo {

MerizosGlobalParams merizosGlobalParams;

void printMerizosHelp(const moe::OptionSection& options) {
    std::cout << options.helpString() << std::endl;
};

bool handlePreValidationMerizosOptions(const moe::Environment& params,
                                      const std::vector<std::string>& args) {
    if (params.count("help") && params["help"].as<bool>() == true) {
        printMerizosHelp(moe::startupOptions);
        return false;
    }
    if (params.count("version") && params["version"].as<bool>() == true) {
        printShardingVersionInfo(true);
        return false;
    }
    if (params.count("test") && params["test"].as<bool>() == true) {
        ::merizo::logger::globalLogDomain()->setMinimumLoggedSeverity(
            ::merizo::logger::LogSeverity::Debug(5));
        StartupTest::runTests();
        return false;
    }

    return true;
}

Status validateMerizosOptions(const moe::Environment& params) {
    Status ret = validateServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

    return Status::OK();
}

Status canonicalizeMerizosOptions(moe::Environment* params) {
    Status ret = canonicalizeServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

    return Status::OK();
}

Status storeMerizosOptions(const moe::Environment& params) {
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

    if (params.count("noscripting") || params.count("security.javascriptEnabled")) {
        warning() << "The Javascript enabled/disabled options are not supported for merizos. "
                     "(\"noscripting\" and/or \"security.javascriptEnabled\" are set.)";
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
                      << escape(configdbConnectionString.getValue().getSetName())
                      << "\" resolves as a host name, but none of the servers in the seed list do. "
                         "Did you reverse the replica set name and the seed list in "
                      << escape(configdbConnectionString.getValue().toString()) << "?";
        }
    }

    merizosGlobalParams.configdbs =
        ConnectionString{configdbConnectionString.getValue().type(),
                         seedServers,
                         configdbConnectionString.getValue().getSetName()};

    if (merizosGlobalParams.configdbs.getServers().size() < 3) {
        warning() << "Running a sharded cluster with fewer than 3 config servers should only be "
                     "done for testing purposes and is not recommended for production.";
    }

    return Status::OK();
}

}  // namespace merizo
