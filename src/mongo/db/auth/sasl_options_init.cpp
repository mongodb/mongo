// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/sasl_options_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/options_parser/value.h"

#include <algorithm>
#include <string>
#include <vector>

#include <boost/algorithm/string/trim.hpp>

namespace mongo {

Status storeSASLOptions(const moe::Environment& params) {
    int scramSHA1IterationCount = saslGlobalParams.scramSHA1IterationCount.load();

    if (params.count("security.authenticationMechanisms") &&
        saslGlobalParams.numTimesAuthenticationMechanismsSet.load() <= 1) {
        saslGlobalParams.authenticationMechanisms =
            params["security.authenticationMechanisms"].as<std::vector<std::string>>();
    }
    if (params.count("security.sasl.hostName") && !saslGlobalParams.haveHostName.load()) {
        saslGlobalParams.hostName = params["security.sasl.hostName"].as<std::string>();
    }
    if (params.count("security.sasl.serviceName") && !saslGlobalParams.haveServiceName.load()) {
        saslGlobalParams.serviceName = params["security.sasl.serviceName"].as<std::string>();
    }
    if (params.count("security.sasl.saslauthdSocketPath") &&
        !saslGlobalParams.haveAuthdPath.load()) {
        saslGlobalParams.authdPath = params["security.sasl.saslauthdSocketPath"].as<std::string>();
    }
    if (params.count("security.sasl.scramIterationCount") &&
        saslGlobalParams.numTimesScramSHA1IterationCountSet.load() <= 1) {
        scramSHA1IterationCount = params["security.sasl.scramIterationCount"].as<int>();
        saslGlobalParams.scramSHA1IterationCount.store(scramSHA1IterationCount);
    }
    if (saslGlobalParams.numTimesScramSHA256IterationCountSet.load() <= 1) {
        if (params.count("security.sasl.scramSHA256IterationCount")) {
            saslGlobalParams.scramSHA256IterationCount.store(
                params["security.sasl.scramSHA256IterationCount"].as<int>());
        } else {
            // If scramSHA256IterationCount isn't provided explicitly,
            // then fall back on scramIterationCount if it is greater than
            // the default scramSHA256IterationCount.
            saslGlobalParams.scramSHA256IterationCount.store(
                std::max<int>(scramSHA1IterationCount, kScramSHA256IterationCountDefault));
        }
    }

    if (saslGlobalParams.hostName.empty())
        saslGlobalParams.hostName = getHostNameCached();
    if (saslGlobalParams.serviceName.empty())
        saslGlobalParams.serviceName = "mongodb";

    // Strip white space for authentication mechanisms
    for (auto& mechanism : saslGlobalParams.authenticationMechanisms) {
        boost::trim(mechanism);
    }

    return Status::OK();
}

MONGO_INITIALIZER_GENERAL(StoreSASLOptions, ("CoreOptions_Store"), ("EndStartupOptionStorage"))
(InitializerContext*) {
    uassertStatusOK(storeSASLOptions(moe::startupOptionsParsed));
}
}  // namespace mongo
