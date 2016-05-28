/*
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/db/server_options_helpers.h"

#ifdef _WIN32
#include <direct.h>
#else
#define SYSLOG_NAMES
#include <syslog.h>
#endif
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <ios>
#include <iostream>

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/logger/log_component.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/log.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/listen.h"  // For DEFAULT_MAX_CONN
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/options_parser/startup_options.h"

using std::endl;
using std::string;

namespace mongo {

/*
 * SERVER-11160 syslog.h does not define facilitynames under solaris.
 * syslog.h exports preprocessor macro INTERNAL_NOPRI if
 * facilitynames is provided. This will be used to determine
 * if facilitynames should be defined here.
 * These could also go into a syslog.h compatibility header.
 * We are using INTERNAL_NOPRI as the indicator macro for facilitynames
 * because it's defined alongside facilitynames in the syslog.h headers
 * that support SYSLOG_NAMES.
 */

namespace {

#if defined(SYSLOG_NAMES)
#if !defined(INTERNAL_NOPRI)

typedef struct _code {
    const char* c_name;
    int c_val;
} CODE;

CODE facilitynames[] = {{"auth", LOG_AUTH},     {"cron", LOG_CRON},     {"daemon", LOG_DAEMON},
                        {"kern", LOG_KERN},     {"lpr", LOG_LPR},       {"mail", LOG_MAIL},
                        {"news", LOG_NEWS},     {"security", LOG_AUTH}, /* DEPRECATED */
                        {"syslog", LOG_SYSLOG}, {"user", LOG_USER},     {"uucp", LOG_UUCP},
                        {"local0", LOG_LOCAL0}, {"local1", LOG_LOCAL1}, {"local2", LOG_LOCAL2},
                        {"local3", LOG_LOCAL3}, {"local4", LOG_LOCAL4}, {"local5", LOG_LOCAL5},
                        {"local6", LOG_LOCAL6}, {"local7", LOG_LOCAL7}, {NULL, -1}};

#endif  // !defined(INTERNAL_NOPRI)
#endif  // defined(SYSLOG_NAMES)


}  // namespace

Status addGeneralServerOptions(moe::OptionSection* options) {
    StringBuilder portInfoBuilder;
    StringBuilder maxConnInfoBuilder;
    std::stringstream unixSockPermsBuilder;

    portInfoBuilder << "specify port number - " << ServerGlobalParams::DefaultDBPort
                    << " by default";
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

    // The verbosity level can be set at startup in the following ways.  Note that if multiple
    // methods for setting the verbosity are specified simultaneously, the verbosity will be set
    // based on the whichever option specifies the highest level
    //
    // Command Line Option | Resulting Verbosity
    // _________________________________________
    // (none)              | 0
    // --verbose ""        | Error after Boost 1.59
    // --verbose           | 1
    // --verbose v         | 1
    // --verbose vv        | 2 (etc.)
    // -v                  | 1
    // -vv                 | 2 (etc.)
    //
    // INI Config Option   | Resulting Verbosity
    // _________________________________________
    // verbose=            | 0
    // verbose=v           | 1
    // verbose=vv          | 2 (etc.)
    // v=true              | 1
    // vv=true             | 2 (etc.)
    //
    // YAML Config Option  | Resulting Verbosity
    // _________________________________________
    // systemLog:          |
    //    verbosity: 5     | 5
    // systemLog:          |
    //   component:        |
    //     verbosity: 5    | 5
    // systemLog:          |
    //   component:        |
    //     Sharding:       |
    //       verbosity: 5  | 5 (for Sharding only, 0 for default)
    options
        ->addOptionChaining(
            "verbose",
            "verbose,v",
            moe::String,
            "be more verbose (include multiple times for more verbosity e.g. -vvvvv)")
        .setImplicit(moe::Value(std::string("v")))
        .setSources(moe::SourceAllLegacy);

    options->addOptionChaining("systemLog.verbosity", "", moe::Int, "set verbose level")
        .setSources(moe::SourceYAMLConfig);

    // log component hierarchy verbosity levels
    for (int i = 0; i < int(logger::LogComponent::kNumLogComponents); ++i) {
        logger::LogComponent component = static_cast<logger::LogComponent::Value>(i);
        if (component == logger::LogComponent::kDefault) {
            continue;
        }
        options
            ->addOptionChaining("systemLog.component." + component.getDottedName() + ".verbosity",
                                "",
                                moe::Int,
                                "set component verbose level for " + component.getDottedName())
            .setSources(moe::SourceYAMLConfig);
    }

    options->addOptionChaining("systemLog.quiet", "quiet", moe::Switch, "quieter output");

    options->addOptionChaining("net.port", "port", moe::Int, portInfoBuilder.str().c_str());

    options->addOptionChaining(
        "net.bindIp",
        "bind_ip",
        moe::String,
        "comma separated list of ip addresses to listen on - all local ips by default");

    options->addOptionChaining(
        "net.ipv6", "ipv6", moe::Switch, "enable IPv6 support (disabled by default)");

    options->addOptionChaining(
        "net.maxIncomingConnections", "maxConns", moe::Int, maxConnInfoBuilder.str().c_str());

    options
        ->addOptionChaining(
            "logpath",
            "logpath",
            moe::String,
            "log file to send write to instead of stdout - has to be a file, not directory")
        .setSources(moe::SourceAllLegacy)
        .incompatibleWith("syslog");

    options
        ->addOptionChaining(
            "systemLog.path",
            "",
            moe::String,
            "log file to send writes to if logging to a file - has to be a file, not directory")
        .setSources(moe::SourceYAMLConfig)
        .hidden();

    options
        ->addOptionChaining("systemLog.destination",
                            "",
                            moe::String,
                            "Destination of system log output.  (syslog/file)")
        .setSources(moe::SourceYAMLConfig)
        .hidden()
        .format("(:?syslog)|(:?file)", "(syslog/file)");

#ifndef _WIN32
    options
        ->addOptionChaining("syslog",
                            "syslog",
                            moe::Switch,
                            "log to system's syslog facility instead of file or stdout")
        .incompatibleWith("logpath")
        .setSources(moe::SourceAllLegacy);

    options->addOptionChaining("systemLog.syslogFacility",
                               "syslogFacility",
                               moe::String,
                               "syslog facility used for mongodb syslog message");

#endif  // _WIN32
    options->addOptionChaining("systemLog.logAppend",
                               "logappend",
                               moe::Switch,
                               "append to logpath instead of over-writing");

    options->addOptionChaining("systemLog.logRotate",
                               "logRotate",
                               moe::String,
                               "set the log rotation behavior (rename|reopen)");

    options->addOptionChaining("systemLog.timeStampFormat",
                               "timeStampFormat",
                               moe::String,
                               "Desired format for timestamps in log messages. One of ctime, "
                               "iso8601-utc or iso8601-local");

    options->addOptionChaining("processManagement.pidFilePath",
                               "pidfilepath",
                               moe::String,
                               "full path to pidfile (if not set, no pidfile is created)");

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
            "setParameter", "setParameter", moe::StringMap, "Set a configurable parameter")
        .composing();

    options
        ->addOptionChaining("httpinterface", "httpinterface", moe::Switch, "enable http interface")
        .setSources(moe::SourceAllLegacy)
        .incompatibleWith("nohttpinterface");

    options->addOptionChaining("net.http.enabled", "", moe::Bool, "enable http interface")
        .setSources(moe::SourceYAMLConfig);

    options
        ->addOptionChaining(
            "net.http.port", "", moe::Switch, "port to listen on for http interface")
        .setSources(moe::SourceYAMLConfig);

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

    /* support for -vv -vvvv etc. */
    for (string s = "vv"; s.length() <= 12; s.append("v")) {
        options->addOptionChaining(s.c_str(), s.c_str(), moe::Switch, "verbose")
            .hidden()
            .setSources(moe::SourceAllLegacy);
    }

    // Extra hidden options
    options
        ->addOptionChaining(
            "nohttpinterface", "nohttpinterface", moe::Switch, "disable http interface")
        .hidden()
        .setSources(moe::SourceAllLegacy)
        .incompatibleWith("httpinterface");

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
        ->addOptionChaining("systemLog.traceAllExceptions",
                            "traceExceptions",
                            moe::Switch,
                            "log stack traces for every exception")
        .hidden();

    options
        ->addOptionChaining("enableExperimentalStorageDetailsCmd",
                            "enableExperimentalStorageDetailsCmd",
                            moe::Switch,
                            "EXPERIMENTAL (UNSUPPORTED). "
                            "Enable command computing aggregate statistics on storage.")
        .hidden()
        .setSources(moe::SourceAllLegacy);

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
        return Status(ErrorCodes::InternalError, "Cannot get binary name: argv array is empty");
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
    char buffer[1024];
#ifdef _WIN32
    verify(_getcwd(buffer, 1000));
#else
    verify(getcwd(buffer, 1000));
#endif
    serverGlobalParams.cwd = buffer;
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
    if (params.count("verbose")) {
        std::string verbosity = params["verbose"].as<std::string>();

        // Skip this for backwards compatibility.  See SERVER-11471.
        if (verbosity != "true") {
            for (std::string::iterator iterator = verbosity.begin(); iterator != verbosity.end();
                 iterator++) {
                if (*iterator != 'v') {
                    return Status(ErrorCodes::BadValue,
                                  "The \"verbose\" option string cannot contain any characters "
                                  "other than \"v\"");
                }
            }
        }
    }

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
    Status ret = validateSSLServerOptions(params);
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
    // "net.wireObjectCheck" comes from the config file, so override it if either "objcheck" or
    // "noobjcheck" are set, since those come from the command line.
    if (params->count("objcheck")) {
        Status ret =
            params->set("net.wireObjectCheck", moe::Value((*params)["objcheck"].as<bool>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("objcheck");
        if (!ret.isOK()) {
            return ret;
        }
    }

    if (params->count("noobjcheck")) {
        Status ret =
            params->set("net.wireObjectCheck", moe::Value(!(*params)["noobjcheck"].as<bool>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("noobjcheck");
        if (!ret.isOK()) {
            return ret;
        }
    }

    // "net.http.enabled" comes from the config file, so override it if "nohttpinterface" or
    // "httpinterface" are set since those come from the command line.
    if (params->count("nohttpinterface")) {
        Status ret =
            params->set("net.http.enabled", moe::Value(!(*params)["nohttpinterface"].as<bool>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("nohttpinterface");
        if (!ret.isOK()) {
            return ret;
        }
    }
    if (params->count("httpinterface")) {
        Status ret =
            params->set("net.http.enabled", moe::Value((*params)["httpinterface"].as<bool>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("httpinterface");
        if (!ret.isOK()) {
            return ret;
        }
    }

    // "net.unixDomainSocket.enabled" comes from the config file, so override it if
    // "nounixsocket" is set since that comes from the command line.
    if (params->count("nounixsocket")) {
        Status ret = params->set("net.unixDomainSocket.enabled",
                                 moe::Value(!(*params)["nounixsocket"].as<bool>()));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("nounixsocket");
        if (!ret.isOK()) {
            return ret;
        }
    }

    // Handle both the "--verbose" string argument and the "-vvvv" arguments at the same time so
    // that we ensure that we set the log level to the maximum of the options provided
    int logLevel = -1;
    for (std::string s = ""; s.length() <= 14; s.append("v")) {
        if (!s.empty() && params->count(s) && (*params)[s].as<bool>() == true) {
            logLevel = s.length();
        }

        if (params->count("verbose")) {
            std::string verbosity;
            params->get("verbose", &verbosity);
            if (s == verbosity ||
                // Treat a verbosity of "true" the same as a single "v".  See SERVER-11471.
                (s == "v" && verbosity == "true")) {
                logLevel = s.length();
            }
        }

        // Remove all "v" options we have already handled
        Status ret = params->remove(s);
        if (!ret.isOK()) {
            return ret;
        }
    }

    if (logLevel != -1) {
        Status ret = params->set("systemLog.verbosity", moe::Value(logLevel));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("verbose");
        if (!ret.isOK()) {
            return ret;
        }
    }

    if (params->count("logpath")) {
        std::string logpath;
        Status ret = params->get("logpath", &logpath);
        if (!ret.isOK()) {
            return ret;
        }
        if (logpath.empty()) {
            return Status(ErrorCodes::BadValue, "logpath cannot be empty if supplied");
        }
        ret = params->set("systemLog.destination", moe::Value(std::string("file")));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->set("systemLog.path", moe::Value(logpath));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("logpath");
        if (!ret.isOK()) {
            return ret;
        }
    }

    // "systemLog.destination" comes from the config file, so override it if "syslog" is set
    // since that comes from the command line.
    if (params->count("syslog") && (*params)["syslog"].as<bool>() == true) {
        Status ret = params->set("systemLog.destination", moe::Value(std::string("syslog")));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("syslog");
        if (!ret.isOK()) {
            return ret;
        }
    }

    if (params->count("noauth")) {
        Status ret =
            params->set("security.authorization",
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

Status storeServerOptions(const moe::Environment& params, const std::vector<std::string>& args) {
    Status ret = setupBinaryName(args);
    if (!ret.isOK()) {
        return ret;
    }

    ret = setupCwd();
    if (!ret.isOK()) {
        return ret;
    }

    ret = setArgvArray(args);
    if (!ret.isOK()) {
        return ret;
    }

    ret = setParsedOpts(params);
    if (!ret.isOK()) {
        return ret;
    }

    // Check options that are not yet supported
    if (params.count("net.http.port")) {
        return Status(ErrorCodes::BadValue, "The net.http.port option is not currently supported");
    }

    if (params.count("systemLog.verbosity")) {
        int verbosity = params["systemLog.verbosity"].as<int>();
        if (verbosity < 0) {
            // This can only happen in YAML config
            return Status(ErrorCodes::BadValue,
                          "systemLog.verbosity YAML Config cannot be negative");
        }
        logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(verbosity));
    }

    // log component hierarchy verbosity levels
    for (int i = 0; i < int(logger::LogComponent::kNumLogComponents); ++i) {
        logger::LogComponent component = static_cast<logger::LogComponent::Value>(i);
        if (component == logger::LogComponent::kDefault) {
            continue;
        }
        const string dottedName = "systemLog.component." + component.getDottedName() + ".verbosity";
        if (params.count(dottedName)) {
            int verbosity = params[dottedName].as<int>();
            // Clear existing log level if log level is negative.
            if (verbosity < 0) {
                logger::globalLogDomain()->clearMinimumLoggedSeverity(component);
            } else {
                logger::globalLogDomain()->setMinimumLoggedSeverity(
                    component, logger::LogSeverity::Debug(verbosity));
            }
        }
    }

    if (params.count("enableExperimentalStorageDetailsCmd")) {
        serverGlobalParams.experimental.storageDetailsCmdEnabled =
            params["enableExperimentalStorageDetailsCmd"].as<bool>();
    }

    if (params.count("net.port")) {
        serverGlobalParams.port = params["net.port"].as<int>();
    }

    if (params.count("net.bindIp")) {
        serverGlobalParams.bind_ip = params["net.bindIp"].as<std::string>();
    }

    if (params.count("net.ipv6") && params["net.ipv6"].as<bool>() == true) {
        enableIPv6();
    }

    if (params.count("net.http.enabled")) {
        serverGlobalParams.isHttpInterfaceEnabled = params["net.http.enabled"].as<bool>();
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

    if (params.count("systemLog.quiet")) {
        serverGlobalParams.quiet = params["systemLog.quiet"].as<bool>();
    }

    if (params.count("systemLog.traceAllExceptions")) {
        DBException::traceExceptions = params["systemLog.traceAllExceptions"].as<bool>();
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

    if (params.count("net.bindIp")) {
        // passing in wildcard is the same as default behavior; remove for SERVER-3350
        if (serverGlobalParams.bind_ip == "0.0.0.0") {
            serverGlobalParams.bind_ip = "";
        }
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

    if (params.count("systemLog.timeStampFormat")) {
        using logger::MessageEventDetailsEncoder;
        std::string formatterName = params["systemLog.timeStampFormat"].as<string>();
        if (formatterName == "ctime") {
            MessageEventDetailsEncoder::setDateFormatter(outputDateAsCtime);
        } else if (formatterName == "iso8601-utc") {
            MessageEventDetailsEncoder::setDateFormatter(outputDateAsISOStringUTC);
        } else if (formatterName == "iso8601-local") {
            MessageEventDetailsEncoder::setDateFormatter(outputDateAsISOStringLocal);
        } else {
            StringBuilder sb;
            sb << "Value of logTimestampFormat must be one of ctime, iso8601-utc "
               << "or iso8601-local; not \"" << formatterName << "\".";
            return Status(ErrorCodes::BadValue, sb.str());
        }
    }
    if (params.count("systemLog.destination")) {
        std::string systemLogDestination = params["systemLog.destination"].as<std::string>();
        if (systemLogDestination == "file") {
            if (params.count("systemLog.path")) {
                serverGlobalParams.logpath = params["systemLog.path"].as<std::string>();
            } else {
                return Status(ErrorCodes::BadValue,
                              "systemLog.path is required if systemLog.destination is to a "
                              "file");
            }
        } else if (systemLogDestination == "syslog") {
            if (params.count("systemLog.path")) {
                return Status(ErrorCodes::BadValue,
                              "Can only use systemLog.path if systemLog.destination is to a "
                              "file");
            }
            serverGlobalParams.logWithSyslog = true;
        } else {
            StringBuilder sb;
            sb << "Bad value for systemLog.destination: " << systemLogDestination
               << ".  Supported targets are: (syslog|file)";
            return Status(ErrorCodes::BadValue, sb.str());
        }
    } else {
        if (params.count("systemLog.path")) {
            return Status(ErrorCodes::BadValue,
                          "Can only use systemLog.path if systemLog.destination is to a file");
        }
    }

#ifndef _WIN32
    if (params.count("systemLog.syslogFacility")) {
        std::string facility = params["systemLog.syslogFacility"].as<string>();
        bool set = false;
        // match facility string to facility value
        size_t facilitynamesLength = sizeof(facilitynames) / sizeof(facilitynames[0]);
        for (unsigned long i = 0; i < facilitynamesLength && facilitynames[i].c_name != NULL; i++) {
            if (!facility.compare(facilitynames[i].c_name)) {
                serverGlobalParams.syslogFacility = facilitynames[i].c_val;
                set = true;
            }
        }
        if (!set) {
            StringBuilder sb;
            sb << "ERROR: syslogFacility must be set to a string representing one of the "
               << "possible syslog facilities";
            return Status(ErrorCodes::BadValue, sb.str());
        }
    } else {
        serverGlobalParams.syslogFacility = LOG_USER;
    }
#endif  // _WIN32

    if (params.count("systemLog.logAppend") && params["systemLog.logAppend"].as<bool>() == true) {
        serverGlobalParams.logAppend = true;
    }

    if (params.count("systemLog.logRotate")) {
        std::string logRotateParam = params["systemLog.logRotate"].as<string>();
        if (logRotateParam == "reopen") {
            serverGlobalParams.logRenameOnRotate = false;

            if (serverGlobalParams.logAppend == false) {
                return Status(ErrorCodes::BadValue,
                              "logAppend must equal true if logRotate is set to reopen");
            }
        } else if (logRotateParam == "rename") {
            serverGlobalParams.logRenameOnRotate = true;
        } else {
            return Status(ErrorCodes::BadValue,
                          "unsupported value for logRotate " + logRotateParam);
        }
    }

    if (!serverGlobalParams.logpath.empty() && serverGlobalParams.logWithSyslog) {
        return Status(ErrorCodes::BadValue, "Cant use both a logpath and syslog ");
    }

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

    if (params.count("setParameter")) {
        std::map<std::string, std::string> parameters =
            params["setParameter"].as<std::map<std::string, std::string>>();
        for (std::map<std::string, std::string>::iterator parametersIt = parameters.begin();
             parametersIt != parameters.end();
             parametersIt++) {
            ServerParameter* parameter =
                mapFindWithDefault(ServerParameterSet::getGlobal()->getMap(),
                                   parametersIt->first,
                                   static_cast<ServerParameter*>(NULL));
            if (NULL == parameter) {
                StringBuilder sb;
                sb << "Illegal --setParameter parameter: \"" << parametersIt->first << "\"";
                return Status(ErrorCodes::BadValue, sb.str());
            }
            if (!parameter->allowedToChangeAtStartup()) {
                StringBuilder sb;
                sb << "Cannot use --setParameter to set \"" << parametersIt->first
                   << "\" at startup";
                return Status(ErrorCodes::BadValue, sb.str());
            }
            Status status = parameter->setFromString(parametersIt->second);
            if (!status.isOK()) {
                StringBuilder sb;
                sb << "Bad value for parameter \"" << parametersIt->first
                   << "\": " << status.reason();
                return Status(ErrorCodes::BadValue, sb.str());
            }
        }
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

    return Status::OK();
}

}  // namespace mongo
