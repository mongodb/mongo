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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/db/auth/sasl_options.h"

#include "mongo/base/status.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

SASLGlobalParams saslGlobalParams;

const int defaultScramIterationCount = 10000;
const int minimumScramIterationCount = 5000;

SASLGlobalParams::SASLGlobalParams() {
    // Authentication mechanisms supported by default.
    authenticationMechanisms.push_back("MONGODB-CR");
    authenticationMechanisms.push_back("MONGODB-X509");
    authenticationMechanisms.push_back("SCRAM-SHA-1");

    // Default iteration count for SCRAM authentication.
    scramIterationCount = defaultScramIterationCount;

    // Default value for auth failed delay
    authFailedDelay = 0;
}

Status addSASLOptions(moe::OptionSection* options) {
    moe::OptionSection saslOptions("SASL Options");

    saslOptions
        .addOptionChaining("security.authenticationMechanisms",
                           "",
                           moe::StringVector,
                           "List of supported authentication mechanisms.  "
                           "Default is MONGODB-CR, SCRAM-SHA-1 and MONGODB-X509.")
        .setSources(moe::SourceYAMLConfig);

    saslOptions
        .addOptionChaining(
            "security.sasl.hostName", "", moe::String, "Fully qualified server domain name")
        .setSources(moe::SourceYAMLConfig);

    saslOptions
        .addOptionChaining("security.sasl.serviceName",
                           "",
                           moe::String,
                           "Registered name of the service using SASL")
        .setSources(moe::SourceYAMLConfig);

    saslOptions
        .addOptionChaining("security.sasl.saslauthdSocketPath",
                           "",
                           moe::String,
                           "Path to Unix domain socket file for saslauthd")
        .setSources(moe::SourceYAMLConfig);

    Status ret = options->addSection(saslOptions);
    if (!ret.isOK()) {
        log() << "Failed to add sasl option section: " << ret.toString();
        return ret;
    }

    return Status::OK();
}

Status storeSASLOptions(const moe::Environment& params) {
    bool haveAuthenticationMechanisms = false;
    bool haveHostName = false;
    bool haveServiceName = false;
    bool haveAuthdPath = false;
    bool haveScramIterationCount = false;

    // Check our setParameter options first so that these values can be properly overridden via
    // the command line even though the options have different names.
    if (params.count("setParameter")) {
        std::map<std::string, std::string> parameters =
            params["setParameter"].as<std::map<std::string, std::string>>();
        for (std::map<std::string, std::string>::iterator parametersIt = parameters.begin();
             parametersIt != parameters.end();
             parametersIt++) {
            if (parametersIt->first == "authenticationMechanisms") {
                haveAuthenticationMechanisms = true;
            } else if (parametersIt->first == "saslHostName") {
                haveHostName = true;
            } else if (parametersIt->first == "saslServiceName") {
                haveServiceName = true;
            } else if (parametersIt->first == "saslauthdPath") {
                haveAuthdPath = true;
            } else if (parametersIt->first == "scramIterationCount") {
                haveScramIterationCount = true;
            }
        }
    }

    if (params.count("security.authenticationMechanisms") && !haveAuthenticationMechanisms) {
        saslGlobalParams.authenticationMechanisms =
            params["security.authenticationMechanisms"].as<std::vector<std::string>>();
    }
    if (params.count("security.sasl.hostName") && !haveHostName) {
        saslGlobalParams.hostName = params["security.sasl.hostName"].as<std::string>();
    }
    if (params.count("security.sasl.serviceName") && !haveServiceName) {
        saslGlobalParams.serviceName = params["security.sasl.serviceName"].as<std::string>();
    }
    if (params.count("security.sasl.saslauthdSocketPath") && !haveAuthdPath) {
        saslGlobalParams.authdPath = params["security.sasl.saslauthdSocketPath"].as<std::string>();
    }
    if (params.count("security.sasl.scramIterationCount") && !haveScramIterationCount) {
        saslGlobalParams.scramIterationCount =
            params["security.sasl.scramIterationCount"].as<int>();
    }

    return Status::OK();
}

MONGO_MODULE_STARTUP_OPTIONS_REGISTER(SASLOptions)(InitializerContext* context) {
    return addSASLOptions(&moe::startupOptions);
}

MONGO_STARTUP_OPTIONS_STORE(SASLOptions)(InitializerContext* context) {
    return storeSASLOptions(moe::startupOptionsParsed);
}

// SASL Startup Parameters, making them settable via setParameter on the command line or in the
// legacy INI config file.  None of these parameters are modifiable at runtime.
ExportedServerParameter<std::vector<std::string>, ServerParameterType::kStartupOnly>
    SASLAuthenticationMechanismsSetting(ServerParameterSet::getGlobal(),
                                        "authenticationMechanisms",
                                        &saslGlobalParams.authenticationMechanisms);

ExportedServerParameter<std::string, ServerParameterType::kStartupOnly> SASLHostNameSetting(
    ServerParameterSet::getGlobal(), "saslHostName", &saslGlobalParams.hostName);

ExportedServerParameter<std::string, ServerParameterType::kStartupOnly> SASLServiceNameSetting(
    ServerParameterSet::getGlobal(), "saslServiceName", &saslGlobalParams.serviceName);

ExportedServerParameter<std::string, ServerParameterType::kStartupOnly> SASLAuthdPathSetting(
    ServerParameterSet::getGlobal(), "saslauthdPath", &saslGlobalParams.authdPath);

const std::string scramIterationCountServerParameter = "scramIterationCount";
class ExportedScramIterationCountParameter
    : public ExportedServerParameter<int, ServerParameterType::kStartupAndRuntime> {
public:
    ExportedScramIterationCountParameter()
        : ExportedServerParameter<int, ServerParameterType::kStartupAndRuntime>(
              ServerParameterSet::getGlobal(),
              scramIterationCountServerParameter,
              &saslGlobalParams.scramIterationCount) {}

    virtual Status validate(const int& newValue) {
        if (newValue < minimumScramIterationCount) {
            return Status(
                ErrorCodes::BadValue,
                mongoutils::str::stream() << "Invalid value for SCRAM iteration count: " << newValue
                                          << " is less than the minimum SCRAM iteration count, "
                                          << minimumScramIterationCount);
        }

        return Status::OK();
    }
} scramIterationCountParam;

}  // namespace mongo
