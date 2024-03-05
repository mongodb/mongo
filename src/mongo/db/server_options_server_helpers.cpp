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


#include "mongo/db/server_options_server_helpers.h"

#include <boost/algorithm/string/constants.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/core/addressof.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/function/function_base.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"
#include <boost/type_index/type_index_facade.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <cstdlib>
#include <iostream>
#include <map>
#include <type_traits>
#include <utility>
#include <variant>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_options_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/net/cidr.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/options_parser/value.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


using std::endl;
using std::string;

namespace moe = ::mongo::optionenvironment;

namespace mongo {

namespace {
// Helpers for option storage
Status setupBinaryName(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        return Status(ErrorCodes::UnknownError, "Cannot get binary name: argv array is empty");
    }

    // setup binary name
    serverGlobalParams.binaryName = argv[0];
    size_t i = serverGlobalParams.binaryName.rfind('/');
    if (i != string::npos) {
        serverGlobalParams.binaryName = serverGlobalParams.binaryName.substr(i + 1);
    }
    return Status::OK();
}

Status setupCwd() {
    // setup cwd
    boost::system::error_code ec;
    boost::filesystem::path cwd = boost::filesystem::current_path(ec);
    if (ec) {
        return Status(ErrorCodes::UnknownError,
                      "Cannot get current working directory: " + ec.message());
    }
    serverGlobalParams.cwd = cwd.string();
    return Status::OK();
}

Status setArgvArray(const std::vector<std::string>& argv) {
    BSONArrayBuilder b;
    std::vector<std::string> censoredArgv = argv;
    cmdline_utils::censorArgsVector(&censoredArgv);
    for (size_t i = 0; i < censoredArgv.size(); i++) {
        b << censoredArgv[i];
    }
    serverGlobalParams.argvArray = b.arr();
    return Status::OK();
}

Status setParsedOpts(const moe::Environment& params) {
    serverGlobalParams.parsedOpts = params.toBSON();
    cmdline_utils::censorBSONObj(&serverGlobalParams.parsedOpts);
    return Status::OK();
}

bool shouldFork(const moe::Environment& params) {
    auto paramYes = [](const moe::Environment& params, const std::string& key) {
        return params.count(key) && params[key].as<bool>();
    };

    auto envVarYes = [](const std::string& envKey) {
        auto envVal = getenv(envKey.c_str());
        return envVal && std::string{envVal} == "1";
    };

    if (paramYes(params, "shutdown")) {
        return false;
    }

    if (envVarYes("MONGODB_CONFIG_OVERRIDE_NOFORK")) {
        LOGV2(7484500,
              "Environment variable MONGODB_CONFIG_OVERRIDE_NOFORK == 1, "
              "overriding \"processManagement.fork\" to false");
        return false;
    }

    if (paramYes(params, "processManagement.fork")) {
        return true;
    }

    return false;
}
}  // namespace

void printCommandLineOpts(std::ostream* os) {
    if (os) {
        *os << format(FMT_STRING("Options set by command line: {}"),
                      tojson(serverGlobalParams.parsedOpts, ExtendedRelaxedV2_0_0, true))
            << std::endl;
    } else {
        LOGV2(21951, "Options set by command line", "options"_attr = serverGlobalParams.parsedOpts);
    }
}

Status validateServerOptions(const moe::Environment& params) {
    Status ret = validateBaseOptions(params);
    if (!ret.isOK())
        return ret;

#ifdef _WIN32
    if (params.count("install") || params.count("reinstall")) {
        if (params.count("logpath") &&
            !boost::filesystem::path(params["logpath"].as<string>()).is_absolute()) {
            return Status(ErrorCodes::BadValue,
                          "logpath requires an absolute file path with Windows services");
        }

        if (params.count("config") &&
            !boost::filesystem::path(params["config"].as<string>()).is_absolute()) {
            return Status(ErrorCodes::BadValue,
                          "config requires an absolute file path with Windows services");
        }

        if (params.count("processManagement.pidFilePath") &&
            !boost::filesystem::path(params["processManagement.pidFilePath"].as<string>())
                 .is_absolute()) {
            return Status(ErrorCodes::BadValue,
                          "pidFilePath requires an absolute file path with Windows services");
        }

        if (params.count("security.keyFile") &&
            !boost::filesystem::path(params["security.keyFile"].as<string>()).is_absolute()) {
            return Status(ErrorCodes::BadValue,
                          "keyFile requires an absolute file path with Windows services");
        }
    }
#endif

    bool haveAuthenticationMechanisms = true;
    bool hasAuthorizationEnabled = false;
    if (params.count("security.authenticationMechanisms") &&
        params["security.authenticationMechanisms"].as<std::vector<std::string>>().empty()) {
        haveAuthenticationMechanisms = false;
    }
    if (params.count("setParameter")) {
        std::map<std::string, std::string> parameters =
            params["setParameter"].as<std::map<std::string, std::string>>();
        auto authMechParameter = parameters.find("authenticationMechanisms");
        if (authMechParameter != parameters.end() && authMechParameter->second.empty()) {
            haveAuthenticationMechanisms = false;
        }

        bool internalValidateFeaturesAsPrimaryUsed =
            parameters.find("internalValidateFeaturesAsPrimary") != parameters.end();
        bool internalValidateFeaturesAsMasterUsed =
            parameters.find("internalValidateFeaturesAsMaster") != parameters.end();

        if (internalValidateFeaturesAsPrimaryUsed || internalValidateFeaturesAsMasterUsed) {
            // Command line options that are disallowed when internalValidateFeaturesAsPrimary or
            // internalValidateFeaturesAsMaster, the deprecated alias, is specified.
            std::string parameterName = internalValidateFeaturesAsPrimaryUsed
                ? "internalValidateFeaturesAsPrimary"
                : "internalValidateFeaturesAsMaster";
            if (params.count("replication.replSet")) {
                return Status(ErrorCodes::BadValue,
                              str::stream() <<  //
                                  "Cannot specify both " + parameterName +
                                      " and replication.replSet");
            }
        }
    }
    if ((params.count("security.authorization") &&
         params["security.authorization"].as<std::string>() == "enabled") ||
        params.count("security.clusterAuthMode") || params.count("security.keyFile") ||
        params.count("auth")) {
        hasAuthorizationEnabled = true;
    }
    if (hasAuthorizationEnabled && !haveAuthenticationMechanisms) {
        return Status(ErrorCodes::BadValue,
                      "Authorization is enabled but no authentication mechanisms are present.");
    }

    return Status::OK();
}

Status canonicalizeServerOptions(moe::Environment* params) {
    Status ret = canonicalizeBaseOptions(params);
    if (!ret.isOK())
        return ret;

    // "net.wireObjectCheck" comes from the config file, so override it if either "objcheck" or
    // "noobjcheck" are set, since those come from the command line.
    if (params->count("objcheck")) {
        ret = params->set("net.wireObjectCheck", moe::Value((*params)["objcheck"].as<bool>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("objcheck");
        if (!ret.isOK()) {
            return ret;
        }
    }

    if (params->count("noobjcheck")) {
        ret = params->set("net.wireObjectCheck", moe::Value(!(*params)["noobjcheck"].as<bool>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("noobjcheck");
        if (!ret.isOK()) {
            return ret;
        }
    }

    // "net.unixDomainSocket.enabled" comes from the config file, so override it if
    // "nounixsocket" is set since that comes from the command line.
    if (params->count("nounixsocket")) {
        ret = params->set("net.unixDomainSocket.enabled",
                          moe::Value(!(*params)["nounixsocket"].as<bool>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("nounixsocket");
        if (!ret.isOK()) {
            return ret;
        }
    }

    if (params->count("noauth")) {
        ret = params->set("security.authorization",
                          (*params)["noauth"].as<bool>() ? moe::Value(std::string("disabled"))
                                                         : moe::Value(std::string("enabled")));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("noauth");
        if (!ret.isOK()) {
            return ret;
        }
    }

    return Status::OK();
}

Status setupServerOptions(const std::vector<std::string>& args) {
    Status ret = setupBinaryName(args);
    if (!ret.isOK()) {
        return ret;
    }

    ret = setupCwd();
    if (!ret.isOK()) {
        return ret;
    }

    ret = setupBaseOptions(args);
    if (!ret.isOK()) {
        return ret;
    }

    return Status::OK();
}

Status storeServerOptions(const moe::Environment& params) {
    Status ret = storeBaseOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

    if (params.count("net.port")) {
        serverGlobalParams.port = params["net.port"].as<int>();
    }

    if (params.count("net.ipv6") && params["net.ipv6"].as<bool>() == true) {
        serverGlobalParams.enableIPv6 = true;
        enableIPv6();
    }

    if (params.count("net.listenBacklog")) {
        serverGlobalParams.listenBacklog = params["net.listenBacklog"].as<int>();
    }

    if (params.count("security.transitionToAuth")) {
        serverGlobalParams.transitionToAuth = params["security.transitionToAuth"].as<bool>();
    }

    if (params.count("security.clusterAuthMode")) {
        const auto modeStr = params["security.clusterAuthMode"].as<std::string>();
        serverGlobalParams.startupClusterAuthMode =
            uassertStatusOK(ClusterAuthMode::parse(modeStr));
        serverGlobalParams.authState = ServerGlobalParams::AuthState::kEnabled;
    }

    if (params.count("net.maxIncomingConnections")) {
        serverGlobalParams.maxConns = params["net.maxIncomingConnections"].as<int>();

        if (serverGlobalParams.maxConns < 5) {
            return Status(ErrorCodes::BadValue, "maxConns has to be at least 5");
        }
    }

    if (params.count("net.maxIncomingConnectionsOverride")) {
        auto ranges = params["net.maxIncomingConnectionsOverride"].as<std::vector<std::string>>();
        for (const auto& range : ranges) {
            auto swr = CIDR::parse(range);
            if (!swr.isOK()) {
                serverGlobalParams.maxConnsOverride.push_back(range);
            } else {
                serverGlobalParams.maxConnsOverride.push_back(std::move(swr.getValue()));
            }
        }
    }

    if (params.count("net.reservedAdminThreads")) {
        serverGlobalParams.reservedAdminThreads = params["net.reservedAdminThreads"].as<int>();
    }

    if (params.count("net.wireObjectCheck")) {
        serverGlobalParams.objcheck = params["net.wireObjectCheck"].as<bool>();
    }

    if (params.count("net.bindIp")) {
        std::string bind_ip = params["net.bindIp"].as<std::string>();
        if (bind_ip == "*") {
            serverGlobalParams.bind_ips.emplace_back("0.0.0.0");
            if (params.count("net.ipv6") && params["net.ipv6"].as<bool>()) {
                serverGlobalParams.bind_ips.emplace_back("::");
            }
        } else {
            boost::split(
                serverGlobalParams.bind_ips,
                bind_ip,
                [](char c) { return c == ','; },
                boost::token_compress_on);
        }
    }

    for (auto& ip : serverGlobalParams.bind_ips) {
        boost::algorithm::trim(ip);
    }

#ifndef _WIN32
    if (params.count("net.unixDomainSocket.pathPrefix")) {
        serverGlobalParams.socket = params["net.unixDomainSocket.pathPrefix"].as<string>();
    }

    if (params.count("net.unixDomainSocket.enabled")) {
        serverGlobalParams.noUnixSocket = !params["net.unixDomainSocket.enabled"].as<bool>();
    }
    if (params.count("net.unixDomainSocket.filePermissions")) {
        serverGlobalParams.unixSocketPermissions =
            params["net.unixDomainSocket.filePermissions"].as<int>();
    }
    if (shouldFork(params)) {
        serverGlobalParams.doFork = true;
    }
#endif  // _WIN32

    if (serverGlobalParams.doFork && serverGlobalParams.logpath.empty() &&
        !serverGlobalParams.logWithSyslog) {
        return Status(ErrorCodes::BadValue, "--fork has to be used with --logpath or --syslog");
    }

    if (params.count("security.keyFile")) {
        serverGlobalParams.keyFile =
            boost::filesystem::absolute(params["security.keyFile"].as<string>()).generic_string();
        serverGlobalParams.authState = ServerGlobalParams::AuthState::kEnabled;
    }

    if (serverGlobalParams.transitionToAuth ||
        (params.count("security.authorization") &&
         params["security.authorization"].as<std::string>() == "disabled")) {
        serverGlobalParams.authState = ServerGlobalParams::AuthState::kDisabled;
    } else if (params.count("security.authorization") &&
               params["security.authorization"].as<std::string>() == "enabled") {
        serverGlobalParams.authState = ServerGlobalParams::AuthState::kEnabled;
    }

    if (params.count("processManagement.pidFilePath")) {
        serverGlobalParams.pidFile = params["processManagement.pidFilePath"].as<string>();
    }

    if (params.count("processManagement.timeZoneInfo")) {
        serverGlobalParams.timeZoneInfoPath = params["processManagement.timeZoneInfo"].as<string>();
    }

    if (!params.count("security.clusterAuthMode") && params.count("security.keyFile")) {
        serverGlobalParams.startupClusterAuthMode = ClusterAuthMode::keyFile();
    }

    const bool hasClusterAuthMode = serverGlobalParams.startupClusterAuthMode.isDefined();
    if (serverGlobalParams.transitionToAuth &&
        !(hasClusterAuthMode &&
          (serverGlobalParams.startupClusterAuthMode.x509Only() ||
           serverGlobalParams.startupClusterAuthMode.keyFileOnly()))) {
        return Status(ErrorCodes::BadValue,
                      "--transitionToAuth must be used with keyFile or x509 authentication");
    }

    if (params.count("net.compression.compressors")) {
        auto ret =
            storeMessageCompressionOptions(params["net.compression.compressors"].as<string>());
        if (!ret.isOK()) {
            return ret;
        }
    }

    return Status::OK();
}

}  // namespace mongo
