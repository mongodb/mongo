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


#include "mongo/platform/basic.h"

#include "mongo/shell/shell_options.h"

#include <boost/filesystem/operations.hpp>

#include <iostream>

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/client_api_version_parameters_gen.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/config.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/shell/shell_utils.h"
#include "mongo/transport/message_compressor_options_client_gen.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/str.h"
#include "mongo/util/version.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

using std::cout;
using std::endl;
using std::string;
using std::vector;

// SERVER-36807: Limit --setShellParameter to SetParameters we know we want to expose.
const std::set<std::string> kSetShellParameterAllowlist = {
    "awsEC2InstanceMetadataUrl",
    "awsECSInstanceMetadataUrl",
    "disabledSecureAllocatorDomains",
    "newLineAfterPasswordPromptForTest",
    "ocspClientHttpTimeoutSecs",
    "ocspEnabled",
    "skipShellCursorFinalize",
    "tlsOCSPSlowResponderWarningSecs",
};

std::string getMongoShellHelp(StringData name, const moe::OptionSection& options) {
    StringBuilder sb;
    sb << "usage: " << name << " [options] [db address] [file names (ending in .js)]\n"
       << "db address can be:\n"
       << "  foo                   foo database on local machine\n"
       << "  192.168.0.5/foo       foo database on 192.168.0.5 machine\n"
       << "  192.168.0.5:9999/foo  foo database on 192.168.0.5 machine on port 9999\n"
       << "  mongodb://192.168.0.5:9999/foo  connection string URI can also be used\n"
       << options.helpString() << "\n"
       << "file names: a list of files to run. files have to end in .js and will exit after "
       << "unless --shell is specified";
    return sb.str();
}

bool handlePreValidationMongoShellOptions(const moe::Environment& params,
                                          const std::vector<std::string>& args) {
    if (params.count("help")) {
        auto&& vii = VersionInfoInterface::instance();
        std::cout << mongoShellVersion(vii) << std::endl;
        std::cout << getMongoShellHelp(args[0], moe::startupOptions) << std::endl;
        return false;
    }
    if (params.count("version")) {
        auto&& vii = VersionInfoInterface::instance();
        std::cout << mongoShellVersion(vii) << std::endl;
        vii.logBuildInfo(&std::cout);
        return false;
    }
    return true;
}

Status storeMongoShellOptions(const moe::Environment& params,
                              const std::vector<std::string>& args) {
    if (params.count("quiet")) {
        mongo::serverGlobalParams.quiet.store(true);
    }

    if (params.count("ipv6")) {
        mongo::enableIPv6();
        shellGlobalParams.enableIPv6 = true;
    }

    auto minimumLoggedSeveity = logv2::LogSeverity::Info();
    if (params.count("verbose")) {
        minimumLoggedSeveity = logv2::LogSeverity::Debug(1);
    }
    logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
        mongo::logv2::LogComponent::kDefault, minimumLoggedSeveity);

    // `objcheck` option is part of `serverGlobalParams` to avoid making common parts depend upon
    // the client options.  The option is set to false in clients by default.
    if (params.count("objcheck")) {
        serverGlobalParams.objcheck = true;
    } else if (params.count("noobjcheck")) {
        serverGlobalParams.objcheck = false;
    } else {
        serverGlobalParams.objcheck = false;
    }

    // Similar to 'objcheck' above, 'crashOnInvalidBSONError' must be common to both the server
    // and shell for linking reasons.
    if (params.count("crashOnInvalidBSONError")) {
        serverGlobalParams.crashOnInvalidBSONError = true;
    }

    if (params.count("port")) {
        shellGlobalParams.port = params["port"].as<string>();
    }

    if (params.count("host")) {
        shellGlobalParams.dbhost = params["host"].as<string>();
    }

    if (params.count("username")) {
        shellGlobalParams.username = params["username"].as<string>();
    }

    if (params.count("password")) {
        shellGlobalParams.password = params["password"].as<string>();
    }

    if (params.count("authenticationDatabase")) {
        shellGlobalParams.authenticationDatabase = params["authenticationDatabase"].as<string>();
    }

    if (params.count("authenticationMechanism")) {
        shellGlobalParams.authenticationMechanism = params["authenticationMechanism"].as<string>();
    }

    if (params.count("gssapiServiceName")) {
        shellGlobalParams.gssapiServiceName = params["gssapiServiceName"].as<string>();
    }

    if (params.count("gssapiHostName")) {
        shellGlobalParams.gssapiHostName = params["gssapiHostName"].as<string>();
    }

    if (params.count("net.compression.compressors")) {
        shellGlobalParams.networkMessageCompressors =
            params["net.compression.compressors"].as<string>();
    }

    if (params.count("nodb")) {
        shellGlobalParams.nodb = true;
    }
    if (params.count("disableJavaScriptProtection")) {
        shellGlobalParams.javascriptProtection = false;
    }
    if (params.count("disableJavaScriptJIT")) {
        shellGlobalParams.nojit = true;
    }
    if (params.count("enableJavaScriptJIT")) {
        shellGlobalParams.nojit = false;
    }
    if (params.count("files")) {
        shellGlobalParams.files = params["files"].as<vector<string>>();
    }
    if (params.count("disableImplicitSessions")) {
        shellGlobalParams.shouldUseImplicitSessions = false;
    }

    /* This is a bit confusing, here are the rules:
     *
     * if nodb is set then all positional parameters are files
     * otherwise the first positional parameter might be a dbaddress, but
     * only if one of these conditions is met:
     *   - it contains no '.' after the last appearance of '\' or '/'
     *   - it doesn't end in '.js' and it doesn't specify a path to an existing file */
    if (params.count("dbaddress")) {
        string dbaddress = params["dbaddress"].as<string>();
        if (shellGlobalParams.nodb) {
            shellGlobalParams.files.insert(shellGlobalParams.files.begin(), dbaddress);
        } else {
            string basename = dbaddress.substr(dbaddress.find_last_of("/\\") + 1);
            if (basename.find_first_of('.') == string::npos ||
                (basename.find(".js", basename.size() - 3) == string::npos &&
                 !::mongo::shell_utils::fileExists(dbaddress))) {
                shellGlobalParams.url = dbaddress;
            } else {
                shellGlobalParams.files.insert(shellGlobalParams.files.begin(), dbaddress);
            }
        }
    }

    if (params.count("jsHeapLimitMB")) {
        int jsHeapLimitMB = params["jsHeapLimitMB"].as<int>();
        if (jsHeapLimitMB <= 0) {
            StringBuilder sb;
            sb << "ERROR: \"jsHeapLimitMB\" needs to be greater than 0";
            return Status(ErrorCodes::BadValue, sb.str());
        }
        shellGlobalParams.jsHeapLimitMB = jsHeapLimitMB;
    }

    if (params.count("idleSessionTimeout")) {
        shellGlobalParams.idleSessionTimeout = Seconds(params["idleSessionTimeout"].as<int>());
    }

    if (shellGlobalParams.url == "*") {
        StringBuilder sb;
        sb << "ERROR: "
           << "\"*\" is an invalid db address";
        sb << getMongoShellHelp(args[0], moe::startupOptions);
        return Status(ErrorCodes::BadValue, sb.str());
    }

    if ((shellGlobalParams.url.find("mongodb://") == 0) ||
        (shellGlobalParams.url.find("mongodb+srv://") == 0)) {
        auto cs_status = MongoURI::parse(shellGlobalParams.url);
        if (!cs_status.isOK()) {
            return cs_status.getStatus();
        }

        auto cs = cs_status.getValue();
        auto uriOptions = cs.getOptions();

        auto handleURIOptions = [&] {
            StringBuilder sb;
            sb << "ERROR: Cannot specify ";

            if (!shellGlobalParams.username.empty() && !cs.getUser().empty() &&
                shellGlobalParams.username != cs.getUser()) {
                sb << "different usernames";
            } else if (!shellGlobalParams.password.empty() && !cs.getPassword().empty() &&
                       shellGlobalParams.password != cs.getPassword()) {
                sb << "different passwords";
            } else if (!shellGlobalParams.authenticationMechanism.empty() &&
                       uriOptions.count("authMechanism") &&
                       uriOptions["authMechanism"] != shellGlobalParams.authenticationMechanism) {
                sb << "different authentication mechanisms";
            } else if (!shellGlobalParams.authenticationDatabase.empty() &&
                       uriOptions.count("authSource") &&
                       uriOptions["authSource"] != shellGlobalParams.authenticationDatabase) {
                sb << "different authentication databases";
            } else if (shellGlobalParams.gssapiServiceName != saslDefaultServiceName &&
                       uriOptions.count("gssapiServiceName")) {
                sb << "the GSSAPI service name";
            } else if (shellGlobalParams.networkMessageCompressors !=
                           kNet_compression_compressorsDefault &&
                       uriOptions.count("compressors") &&
                       uriOptions["compressors"] != shellGlobalParams.networkMessageCompressors) {
                sb << "different network message compressors";
            } else {
                return Status::OK();
            }

            sb << " in connection URI and as a command-line option";
            return Status(ErrorCodes::InvalidOptions, sb.str());
        };

        auto uriStatus = handleURIOptions();
        if (!uriStatus.isOK())
            return uriStatus;

        if (uriOptions.count("compressors"))
            shellGlobalParams.networkMessageCompressors = uriOptions["compressors"];
    }

    if (!shellGlobalParams.networkMessageCompressors.empty()) {
        const auto ret =
            storeMessageCompressionOptions(shellGlobalParams.networkMessageCompressors);
        if (!ret.isOK()) {
            return ret;
        }
    }

    // Future API versions may require logic changes in the shell, so ban them for now.
    if (!shellGlobalParams.apiVersion.empty() && shellGlobalParams.apiVersion != "1") {
        uasserted(4938003,
                  str::stream() << "Bad value --apiVersion '" << shellGlobalParams.apiVersion
                                << "', only API Version 1 is supported");
    }

    if (params.count("setShellParameter")) {
        auto ssp = params["setShellParameter"].as<std::map<std::string, std::string>>();
        auto* paramSet = ServerParameterSet::getNodeParameterSet();
        for (auto it : ssp) {
            const auto& name = it.first;
            auto param = paramSet->getIfExists(name);
            if (!param || !kSetShellParameterAllowlist.count(name)) {
                return {ErrorCodes::BadValue,
                        str::stream() << "Unknown --setShellParameter '" << name << "'"};
            }
            if (!param->allowedToChangeAtStartup()) {
                return {ErrorCodes::BadValue,
                        str::stream()
                            << "Cannot use --setShellParameter to set '" << name << "' at startup"};
            }
            auto status = param->setFromString(it.second);
            if (!status.isOK()) {
                return {ErrorCodes::BadValue,
                        str::stream()
                            << "Bad value for parameter '" << name << "': " << status.reason()};
            }
        }
    }

    return Status::OK();
}

std::string getApiParametersJSON() {
    BSONObjBuilder bob;
    if (!shellGlobalParams.apiVersion.empty()) {
        bob.append(ClientAPIVersionParameters::kVersionFieldName, shellGlobalParams.apiVersion);
    }

    if (shellGlobalParams.apiStrict) {
        bob.append(ClientAPIVersionParameters::kStrictFieldName, true);
    }

    if (shellGlobalParams.apiDeprecationErrors) {
        bob.append(ClientAPIVersionParameters::kDeprecationErrorsFieldName, true);
    }

    return bob.done().jsonString();
}

}  // namespace mongo
