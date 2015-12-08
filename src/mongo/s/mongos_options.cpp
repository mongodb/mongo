/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
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
#include "mongo/db/server_options.h"
#include "mongo/db/server_options_helpers.h"
#include "mongo/s/chunk.h"
#include "mongo/s/version_mongos.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/startup_test.h"

namespace mongo {

MongosGlobalParams mongosGlobalParams;

Status addMongosOptions(moe::OptionSection* options) {
    moe::OptionSection general_options("General options");

    Status ret = addGeneralServerOptions(&general_options);
    if (!ret.isOK()) {
        return ret;
    }

#if defined(_WIN32)
    moe::OptionSection windows_scm_options("Windows Service Control Manager options");

    ret = addWindowsServerOptions(&windows_scm_options);
    if (!ret.isOK()) {
        return ret;
    }
#endif

#ifdef MONGO_CONFIG_SSL
    moe::OptionSection ssl_options("SSL options");

    ret = addSSLServerOptions(&ssl_options);
    if (!ret.isOK()) {
        return ret;
    }
#endif

    moe::OptionSection sharding_options("Sharding options");

    sharding_options.addOptionChaining(
        "sharding.configDB",
        "configdb",
        moe::String,
        "Connection string for communicating with config servers. Acceptable forms:\n"
        "CSRS: <config replset name>/<host1:port>,<host2:port>,[...]\n"
        "SCCC (deprecated): <host1:port>,<host2:port>,<host3:port>\n"
        "Single-node (for testing only): <host1:port>");

    sharding_options.addOptionChaining(
        "replication.localPingThresholdMs",
        "localThreshold",
        moe::Int,
        "ping time (in ms) for a node to be considered local (default 15ms)");

    sharding_options.addOptionChaining("test", "test", moe::Switch, "just run unit tests")
        .setSources(moe::SourceAllLegacy);

    sharding_options.addOptionChaining(
        "sharding.chunkSize", "chunkSize", moe::Int, "maximum amount of data per chunk");

    sharding_options.addOptionChaining("net.http.JSONPEnabled",
                                       "jsonp",
                                       moe::Switch,
                                       "allow JSONP access via http (has security implications)")
        .setSources(moe::SourceAllLegacy);

    sharding_options.addOptionChaining(
                         "noscripting", "noscripting", moe::Switch, "disable scripting engine")
        .setSources(moe::SourceAllLegacy);


    options->addSection(general_options);

#if defined(_WIN32)
    options->addSection(windows_scm_options);
#endif

    options->addSection(sharding_options);

#ifdef MONGO_CONFIG_SSL
    options->addSection(ssl_options);
#endif

    options->addOptionChaining("noAutoSplit",
                               "noAutoSplit",
                               moe::Switch,
                               "do not send split commands with writes")
        .hidden()
        .setSources(moe::SourceAllLegacy);

    options->addOptionChaining(
                 "sharding.autoSplit", "", moe::Bool, "send split commands with writes")
        .setSources(moe::SourceYAMLConfig);


    return Status::OK();
}

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
        ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
            ::mongo::logger::LogSeverity::Debug(5));
        StartupTest::runTests();
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

#ifdef MONGO_CONFIG_SSL
    ret = canonicalizeSSLServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }
#endif

    // "sharding.autoSplit" comes from the config file, so override it if "noAutoSplit" is set
    // since that comes from the command line.
    if (params->count("noAutoSplit")) {
        Status ret =
            params->set("sharding.autoSplit", moe::Value(!(*params)["noAutoSplit"].as<bool>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("noAutoSplit");
        if (!ret.isOK()) {
            return ret;
        }
    }

    return Status::OK();
}

Status storeMongosOptions(const moe::Environment& params, const std::vector<std::string>& args) {
    Status ret = storeServerOptions(params, args);
    if (!ret.isOK()) {
        return ret;
    }

    if (params.count("sharding.chunkSize")) {
        int csize = params["sharding.chunkSize"].as<int>();

        // validate chunksize before proceeding
        if (csize == 0) {
            return Status(ErrorCodes::BadValue, "error: need a non-zero chunksize");
        }

        if (!Chunk::setMaxChunkSizeSizeMB(csize)) {
            return Status(ErrorCodes::BadValue, "MaxChunkSize invalid");
        }
    }

    if (params.count("net.port")) {
        int port = params["net.port"].as<int>();
        if (port <= 0 || port > 65535) {
            return Status(ErrorCodes::BadValue, "error: port number must be between 1 and 65535");
        }
    }

    if (params.count("replication.localPingThresholdMs")) {
        serverGlobalParams.defaultLocalThresholdMillis =
            params["replication.localPingThresholdMs"].as<int>();
    }

    if (params.count("net.http.JSONPEnabled")) {
        serverGlobalParams.jsonp = params["net.http.JSONPEnabled"].as<bool>();
    }

    if (params.count("noscripting")) {
        // This option currently has no effect for mongos
    }

    if (params.count("sharding.autoSplit")) {
        Chunk::ShouldAutoSplit = params["sharding.autoSplit"].as<bool>();
        if (Chunk::ShouldAutoSplit == false) {
            warning() << "running with auto-splitting disabled";
        }
    }

    if (!params.count("sharding.configDB")) {
        return Status(ErrorCodes::BadValue, "error: no args for --configdb");
    }

    {
        std::string configdbString = params["sharding.configDB"].as<std::string>();

        auto configdbConnectionString = ConnectionString::parse(configdbString);
        if (!configdbConnectionString.isOK()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Invalid configdb connection string: "
                                        << configdbConnectionString.getStatus().toString());
        }

        std::vector<HostAndPort> seedServers;
        for (const auto& host : configdbConnectionString.getValue().getServers()) {
            seedServers.push_back(host);
            if (!seedServers.back().hasPort()) {
                seedServers.back() = HostAndPort{host.host(), ServerGlobalParams::ConfigServerPort};
            }
        }

        mongosGlobalParams.configdbs =
            ConnectionString{configdbConnectionString.getValue().type(),
                             seedServers,
                             configdbConnectionString.getValue().getSetName()};
    }

    std::vector<HostAndPort> configServers = mongosGlobalParams.configdbs.getServers();

    if (mongosGlobalParams.configdbs.type() != ConnectionString::SYNC &&
        mongosGlobalParams.configdbs.type() != ConnectionString::SET &&
        mongosGlobalParams.configdbs.type() != ConnectionString::MASTER) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid config server value "
                                    << mongosGlobalParams.configdbs.toString());
    }

    if (configServers.size() < 3) {
        warning() << "Running a sharded cluster with fewer than 3 config servers should only be "
                     "done for testing purposes and is not recommended for production.";
    }

    return Status::OK();
}

}  // namespace mongo
