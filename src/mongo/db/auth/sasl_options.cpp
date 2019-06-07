
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/db/auth/sasl_options.h"

#include <boost/algorithm/string.hpp>

#include "mongo/base/status.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

SASLGlobalParams saslGlobalParams;

// For backward compatability purposes, "scramIterationCount" refers to the SHA-1 variant.
// The SHA-256 variant, as well as all future parameters, will use their specific name.
constexpr auto scramSHA1IterationCountServerParameter = "scramIterationCount"_sd;
constexpr auto scramSHA256IterationCountServerParameter = "scramSHA256IterationCount"_sd;

const int defaultScramSHA1IterationCount = 10000;
const int minimumScramSHA1IterationCount = 5000;

const int defaultScramSHA256IterationCount = 15000;
const int minimumScramSHA256IterationCount = 5000;

SASLGlobalParams::SASLGlobalParams() {
    // Authentication mechanisms supported by default.
    authenticationMechanisms.push_back("MONGODB-X509");
    authenticationMechanisms.push_back("SCRAM-SHA-1");
    authenticationMechanisms.push_back("SCRAM-SHA-256");

    // Default iteration count for SCRAM authentication.
    scramSHA1IterationCount.store(defaultScramSHA1IterationCount);
    scramSHA256IterationCount.store(defaultScramSHA256IterationCount);

    // Default value for auth failed delay
    authFailedDelay.store(0);
}

Status addSASLOptions(moe::OptionSection* options) {
    moe::OptionSection saslOptions("SASL Options");

    saslOptions
        .addOptionChaining("security.authenticationMechanisms",
                           "",
                           moe::StringVector,
                           "List of supported authentication mechanisms.  "
                           "Default is SCRAM-SHA-1 and MONGODB-X509.")
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
    bool haveScramSHA1IterationCount = false;
    bool haveScramSHA256IterationCount = false;
    int scramSHA1IterationCount = defaultScramSHA1IterationCount;

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
            } else if (parametersIt->first == scramSHA1IterationCountServerParameter) {
                haveScramSHA1IterationCount = true;
                // If the value here is non-numeric, atoi() will fail to parse.
                // We can ignore that error since the ExportedServerParameter
                // will catch it for us.
                scramSHA1IterationCount = atoi(parametersIt->second.c_str());
            } else if (parametersIt->first == scramSHA256IterationCountServerParameter) {
                haveScramSHA256IterationCount = true;
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
    if (params.count("security.sasl.scramIterationCount") && !haveScramSHA1IterationCount) {
        scramSHA1IterationCount = params["security.sasl.scramIterationCount"].as<int>();
        saslGlobalParams.scramSHA1IterationCount.store(scramSHA1IterationCount);
    }
    if (!haveScramSHA256IterationCount) {
        if (params.count("security.sasl.scramSHA256IterationCount")) {
            saslGlobalParams.scramSHA256IterationCount.store(
                params["security.sasl.scramSHA256IterationCount"].as<int>());
        } else {
            // If scramSHA256IterationCount isn't provided explicitly,
            // then fall back on scramIterationCount if it is greater than
            // the default scramSHA256IterationCount.
            saslGlobalParams.scramSHA256IterationCount.store(
                std::max<int>(scramSHA1IterationCount, defaultScramSHA256IterationCount));
        }
    }

    if (saslGlobalParams.hostName.empty())
        saslGlobalParams.hostName = getHostNameCached();
    if (saslGlobalParams.serviceName.empty())
        saslGlobalParams.serviceName = "mongodb";

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
ExportedServerParameter<std::string, ServerParameterType::kStartupOnly> SASLHostNameSetting(
    ServerParameterSet::getGlobal(), "saslHostName", &saslGlobalParams.hostName);

ExportedServerParameter<std::string, ServerParameterType::kStartupOnly> SASLServiceNameSetting(
    ServerParameterSet::getGlobal(), "saslServiceName", &saslGlobalParams.serviceName);

ExportedServerParameter<std::string, ServerParameterType::kStartupOnly> SASLAuthdPathSetting(
    ServerParameterSet::getGlobal(), "saslauthdPath", &saslGlobalParams.authdPath);

class ExportedScramIterationCountParameter
    : public ExportedServerParameter<int, ServerParameterType::kStartupAndRuntime> {
public:
    ExportedScramIterationCountParameter(StringData name, AtomicInt32* value, int minimum)
        : ExportedServerParameter<int, ServerParameterType::kStartupAndRuntime>(
              ServerParameterSet::getGlobal(), name.toString(), value),
          _minimum(minimum) {}

    virtual Status validate(const int& newValue) {
        if (newValue < _minimum) {
            return Status(
                ErrorCodes::BadValue,
                mongoutils::str::stream() << "Invalid value for SCRAM iteration count: " << newValue
                                          << " is less than the minimum SCRAM iteration count, "
                                          << _minimum);
        }

        return Status::OK();
    }

private:
    int _minimum;
};

class ExportedAuthenticationMechanismParameter
    : public ExportedServerParameter<std::vector<std::string>, ServerParameterType::kStartupOnly> {
public:
    ExportedAuthenticationMechanismParameter(StringData name, std::vector<std::string>* value)
        : ExportedServerParameter<std::vector<std::string>, ServerParameterType::kStartupOnly>(
              ServerParameterSet::getGlobal(), name.toString(), value) {}

    Status setFromString(const std::string& str) final {

        std::vector<std::string> v;
        splitStringDelim(str, &v, ',');

        // Strip white space for authentication mechanisms
        for (auto& mechanism : v) {
            boost::trim(mechanism);
        }

        std::string joinedString = boost::algorithm::join(v, ",");
        return ExportedServerParameter<
            std::vector<std::string>,
            ServerParameterType::kStartupOnly>::setFromString(joinedString);
    }
};

ExportedScramIterationCountParameter scramSHA1IterationCountParam(
    scramSHA1IterationCountServerParameter,
    &saslGlobalParams.scramSHA1IterationCount,
    minimumScramSHA1IterationCount);
ExportedScramIterationCountParameter scramSHA256IterationCountParam(
    scramSHA256IterationCountServerParameter,
    &saslGlobalParams.scramSHA256IterationCount,
    minimumScramSHA256IterationCount);

// modify the input to remove leading and trailing white space
ExportedAuthenticationMechanismParameter authMechanismsParam(
    "authenticationMechanisms", &saslGlobalParams.authenticationMechanisms);

}  // namespace mongo
