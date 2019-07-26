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
#include "mongo/db/auth/sasl_options_gen.h"

#include <boost/algorithm/string.hpp>

#include "mongo/base/status.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/str.h"

namespace mongo {

Status storeSASLOptions(const moe::Environment& params) {
    int scramSHA1IterationCount = saslGlobalParams.scramSHA1IterationCount.load();

    if (params.count("security.authenticationMechanisms") &&
        saslGlobalParams.numTimesAuthenticationMechanismsSet <= 1) {
        saslGlobalParams.authenticationMechanisms =
            params["security.authenticationMechanisms"].as<std::vector<std::string>>();
    }
    if (params.count("security.sasl.hostName") && !saslGlobalParams.haveHostName) {
        saslGlobalParams.hostName = params["security.sasl.hostName"].as<std::string>();
    }
    if (params.count("security.sasl.serviceName") && !saslGlobalParams.haveServiceName) {
        saslGlobalParams.serviceName = params["security.sasl.serviceName"].as<std::string>();
    }
    if (params.count("security.sasl.saslauthdSocketPath") && !saslGlobalParams.haveAuthdPath) {
        saslGlobalParams.authdPath = params["security.sasl.saslauthdSocketPath"].as<std::string>();
    }
    if (params.count("security.sasl.scramIterationCount") &&
        saslGlobalParams.numTimesScramSHA1IterationCountSet <= 1) {
        scramSHA1IterationCount = params["security.sasl.scramIterationCount"].as<int>();
        saslGlobalParams.scramSHA1IterationCount.store(scramSHA1IterationCount);
    }
    if (saslGlobalParams.numTimesScramSHA256IterationCountSet <= 1) {
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
(InitializerContext* const context) {
    return storeSASLOptions(moe::startupOptionsParsed);
}
}  // namespace mongo
