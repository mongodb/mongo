/*
 *    Copyright (C) 2013 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/db/server_options_server_helpers.h"

#include <algorithm>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <ios>
#include <iostream>

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_options_helpers.h"
#include "mongo/db/server_parameters.h"
#include "mongo/logger/log_component.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/options_parser/startup_options.h"

using std::endl;
using std::string;

namespace mongo {

Status addGeneralServerOptions(moe::OptionSection* options) {
    auto baseResult = addBaseServerOptions(options);
    if (!baseResult.isOK()) {
        return baseResult;
    }

    StringBuilder maxConnInfoBuilder;
    std::stringstream unixSockPermsBuilder;

    maxConnInfoBuilder << "max number of simultaneous connections - " << DEFAULT_MAX_CONN
                       << " by default";
    unixSockPermsBuilder << "permissions to set on UNIX domain socket file - "
                         << "0" << std::oct << DEFAULT_UNIX_PERMS << " by default";

    options->addOptionChaining("help", "help,h", moe::Switch, "show this usage information")
        .setSources(moe::SourceAllLegacy);

    options->addOptionChaining("version", "version", moe::Switch, "show version information")
        .setSources(moe::SourceAllLegacy);

    options
        ->addOptionChaining(
            "config", "config,f", moe::String, "configuration file specifying additional options")
        .setSources(moe::SourceAllLegacy);


    options
        ->addOptionChaining(
            "net.bindIp",
            "bind_ip",
            moe::String,
            "comma separated list of ip addresses to listen on - localhost by default")
        .incompatibleWith("bind_ip_all");

    options
        ->addOptionChaining("net.bindIpAll", "bind_ip_all", moe::Switch, "bind to all ip addresses")
        .incompatibleWith("bind_ip");

    options->addOptionChaining(
        "net.ipv6", "ipv6", moe::Switch, "enable IPv6 support (disabled by default)");

    options
        ->addOptionChaining(
            "net.listenBacklog", "listenBacklog", moe::Int, "set socket listen backlog size")
        .setDefault(moe::Value(SOMAXCONN));

    options->addOptionChaining(
        "net.maxIncomingConnections", "maxConns", moe::Int, maxConnInfoBuilder.str().c_str());

    options
        ->addOptionChaining("net.transportLayer",
                            "transportLayer",
                            moe::String,
                            "sets the ingress transport layer implementation")
        .hidden()
        .setDefault(moe::Value("asio"));

    options
        ->addOptionChaining("net.serviceExecutor",
                            "serviceExecutor",
                            moe::String,
                            "sets the service executor implementation")
        .hidden()
        .setDefault(moe::Value("synchronous"));

#if MONGO_ENTERPRISE_VERSION
    options->addOptionChaining("security.redactClientLogData",
                               "redactClientLogData",
                               moe::Switch,
                               "Redact client data written to the diagnostics log");
#endif

    options->addOptionChaining("processManagement.pidFilePath",
                               "pidfilepath",
                               moe::String,
                               "full path to pidfile (if not set, no pidfile is created)");

    options->addOptionChaining("processManagement.timeZoneInfo",
                               "timeZoneInfo",
                               moe::String,
                               "full path to time zone info directory, e.g. /usr/share/zoneinfo");

    options
        ->addOptionChaining(
            "security.keyFile", "keyFile", moe::String, "private key for cluster authentication")
        .incompatibleWith("noauth");

    options->addOptionChaining("noauth", "noauth", moe::Switch, "run without security")
        .setSources(moe::SourceAllLegacy)
        .incompatibleWith("auth")
        .incompatibleWith("keyFile")
        .incompatibleWith("transitionToAuth")
        .incompatibleWith("clusterAuthMode");

    options
        ->addOptionChaining(
            "security.transitionToAuth",
            "transitionToAuth",
            moe::Switch,
            "For rolling access control upgrade. Attempt to authenticate over outgoing "
            "connections and proceed regardless of success. Accept incoming connections "
            "with or without authentication.")
        .incompatibleWith("noauth");

    options
        ->addOptionChaining("security.clusterAuthMode",
                            "clusterAuthMode",
                            moe::String,
                            "Authentication mode used for cluster authentication. Alternatives are "
                            "(keyFile|sendKeyFile|sendX509|x509)")
        .format("(:?keyFile)|(:?sendKeyFile)|(:?sendX509)|(:?x509)",
                "(keyFile/sendKeyFile/sendX509/x509)");

#ifndef _WIN32
    options
        ->addOptionChaining(
            "nounixsocket", "nounixsocket", moe::Switch, "disable listening on unix sockets")
        .setSources(moe::SourceAllLegacy);

    options
        ->addOptionChaining(
            "net.unixDomainSocket.enabled", "", moe::Bool, "disable listening on unix sockets")
        .setSources(moe::SourceYAMLConfig);

    options->addOptionChaining("net.unixDomainSocket.pathPrefix",
                               "unixSocketPrefix",
                               moe::String,
                               "alternative directory for UNIX domain sockets (defaults to /tmp)");

    options->addOptionChaining("net.unixDomainSocket.filePermissions",
                               "filePermissions",
                               moe::Int,
                               unixSockPermsBuilder.str());

    options->addOptionChaining(
        "processManagement.fork", "fork", moe::Switch, "fork server process");

#endif

    options
        ->addOptionChaining("objcheck",
                            "objcheck",
                            moe::Switch,
                            "inspect client data for validity on receipt (DEFAULT)")
        .hidden()
        .setSources(moe::SourceAllLegacy)
        .incompatibleWith("noobjcheck");

    options
        ->addOptionChaining("noobjcheck",
                            "noobjcheck",
                            moe::Switch,
                            "do NOT inspect client data for validity on receipt")
        .hidden()
        .setSources(moe::SourceAllLegacy)
        .incompatibleWith("objcheck");

    options
        ->addOptionChaining("net.wireObjectCheck",
                            "",
                            moe::Bool,
                            "inspect client data for validity on receipt (DEFAULT)")
        .hidden()
        .setSources(moe::SourceYAMLConfig);

    options
        ->addOptionChaining("enableExperimentalStorageDetailsCmd",
                            "enableExperimentalStorageDetailsCmd",
                            moe::Switch,
                            "EXPERIMENTAL (UNSUPPORTED). "
                            "Enable command computing aggregate statistics on storage.")
        .hidden()
        .setSources(moe::SourceAllLegacy);

    options
        ->addOptionChaining("operationProfiling.slowOpThresholdMs",
                            "slowms",
                            moe::Int,
                            "value of slow for profile and console log")
        .setDefault(moe::Value(100));

    options
        ->addOptionChaining("operationProfiling.slowOpSampleRate",
                            "slowOpSampleRate",
                            moe::Double,
                            "fraction of slow ops to include in the profile and console log")
        .setDefault(moe::Value(1.0));

    auto ret = addMessageCompressionOptions(options, false);
    if (!ret.isOK()) {
        return ret;
    }

    return Status::OK();
}

Status addWindowsServerOptions(moe::OptionSection* options) {
    options->addOptionChaining("install", "install", moe::Switch, "install Windows service")
        .setSources(moe::SourceAllLegacy);

    options->addOptionChaining("remove", "remove", moe::Switch, "remove Windows service")
        .setSources(moe::SourceAllLegacy);

    options
        ->addOptionChaining(
            "reinstall",
            "reinstall",
            moe::Switch,
            "reinstall Windows service (equivalent to --remove followed by --install)")
        .setSources(moe::SourceAllLegacy);

    options->addOptionChaining("processManagement.windowsService.serviceName",
                               "serviceName",
                               moe::String,
                               "Windows service name");

    options->addOptionChaining("processManagement.windowsService.displayName",
                               "serviceDisplayName",
                               moe::String,
                               "Windows service display name");

    options->addOptionChaining("processManagement.windowsService.description",
                               "serviceDescription",
                               moe::String,
                               "Windows service description");

    options->addOptionChaining("processManagement.windowsService.serviceUser",
                               "serviceUser",
                               moe::String,
                               "account for service execution");

    options->addOptionChaining("processManagement.windowsService.servicePassword",
                               "servicePassword",
                               moe::String,
                               "password used to authenticate serviceUser");

    options->addOptionChaining("service", "service", moe::Switch, "start mongodb service")
        .hidden()
        .setSources(moe::SourceAllLegacy);

    return Status::OK();
}

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
}  // namespace

void printCommandLineOpts() {
    log() << "options: " << serverGlobalParams.parsedOpts << endl;
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

#ifdef MONGO_CONFIG_SSL
    ret = validateSSLServerOptions(params);
    if (!ret.isOK()) {
        return ret;
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

        if (parameters.find("internalValidateFeaturesAsMaster") != parameters.end()) {
            // Command line options that are disallowed when internalValidateFeaturesAsMaster is
            // specified.
            if (params.count("replication.replSet")) {
                return Status(ErrorCodes::BadValue,
                              str::stream() <<  //
                                  "Cannot specify both internalValidateFeaturesAsMaster and "
                                  "replication.replSet");
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

    if (params.count("net.transportLayer")) {
        serverGlobalParams.transportLayer = params["net.transportLayer"].as<std::string>();
        if (serverGlobalParams.transportLayer != "asio") {
            return {ErrorCodes::BadValue, "Unsupported value for transportLayer. Must be \"asio\""};
        }
    }

    if (params.count("net.serviceExecutor")) {
        auto value = params["net.serviceExecutor"].as<std::string>();
        const auto valid = {"synchronous"_sd, "adaptive"_sd};
        if (std::find(valid.begin(), valid.end(), value) == valid.end()) {
            return {ErrorCodes::BadValue, "Unsupported value for serviceExecutor"};
        }
        serverGlobalParams.serviceExecutor = value;
    } else {
        serverGlobalParams.serviceExecutor = "synchronous";
    }

    if (params.count("security.transitionToAuth")) {
        serverGlobalParams.transitionToAuth = params["security.transitionToAuth"].as<bool>();
    }

    if (params.count("security.clusterAuthMode")) {
        std::string clusterAuthMode = params["security.clusterAuthMode"].as<std::string>();

        if (clusterAuthMode == "keyFile") {
            serverGlobalParams.clusterAuthMode.store(ServerGlobalParams::ClusterAuthMode_keyFile);
        } else if (clusterAuthMode == "sendKeyFile") {
            serverGlobalParams.clusterAuthMode.store(
                ServerGlobalParams::ClusterAuthMode_sendKeyFile);
        } else if (clusterAuthMode == "sendX509") {
            serverGlobalParams.clusterAuthMode.store(ServerGlobalParams::ClusterAuthMode_sendX509);
        } else if (clusterAuthMode == "x509") {
            serverGlobalParams.clusterAuthMode.store(ServerGlobalParams::ClusterAuthMode_x509);
        } else {
            return Status(ErrorCodes::BadValue,
                          "unsupported value for clusterAuthMode " + clusterAuthMode);
        }
        serverGlobalParams.authState = ServerGlobalParams::AuthState::kEnabled;
    } else {
        serverGlobalParams.clusterAuthMode.store(ServerGlobalParams::ClusterAuthMode_undefined);
    }

    if (params.count("net.maxIncomingConnections")) {
        serverGlobalParams.maxConns = params["net.maxIncomingConnections"].as<int>();

        if (serverGlobalParams.maxConns < 5) {
            return Status(ErrorCodes::BadValue, "maxConns has to be at least 5");
        }
    }

    if (params.count("net.wireObjectCheck")) {
        serverGlobalParams.objcheck = params["net.wireObjectCheck"].as<bool>();
    }

    if (params.count("net.bindIpAll") && params["net.bindIpAll"].as<bool>()) {
        // Bind to all IP addresses
        serverGlobalParams.bind_ips.emplace_back("0.0.0.0");
        if (params.count("net.ipv6") && params["net.ipv6"].as<bool>()) {
            serverGlobalParams.bind_ips.emplace_back("::");
        }
    } else if (params.count("net.bindIp")) {
        std::string bind_ip = params["net.bindIp"].as<std::string>();
        boost::split(serverGlobalParams.bind_ips,
                     bind_ip,
                     [](char c) { return c == ','; },
                     boost::token_compress_on);
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

    if ((params.count("processManagement.fork") &&
         params["processManagement.fork"].as<bool>() == true) &&
        (!params.count("shutdown") || params["shutdown"].as<bool>() == false)) {
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
        serverGlobalParams.clusterAuthMode.store(ServerGlobalParams::ClusterAuthMode_keyFile);
    }
    int clusterAuthMode = serverGlobalParams.clusterAuthMode.load();
    if (serverGlobalParams.transitionToAuth &&
        (clusterAuthMode != ServerGlobalParams::ClusterAuthMode_keyFile &&
         clusterAuthMode != ServerGlobalParams::ClusterAuthMode_x509)) {
        return Status(ErrorCodes::BadValue,
                      "--transitionToAuth must be used with keyFile or x509 authentication");
    }

#ifdef MONGO_CONFIG_SSL
    ret = storeSSLServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }
#endif

    ret = storeMessageCompressionOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

    return Status::OK();
}

}  // namespace mongo
