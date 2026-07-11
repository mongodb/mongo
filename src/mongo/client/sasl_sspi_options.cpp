// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#ifdef _WIN32

#include "mongo/client/sasl_sspi_options.h"

#include "mongo/base/status.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

#include <string>
#include <vector>

namespace mongo {

SASLSSPIGlobalParams saslSSPIGlobalParams;

Status storeSASLSSPIOptions(const moe::Environment& params) {
    if (params.count("security.sspiHostnameCanonicalization")) {
        if (params["security.sspiHostnameCanonicalization"].as<std::string>() == "none") {
            saslSSPIGlobalParams.canonicalization = HostnameCanonicalizationMode::kNone;
        } else if (params["security.sspiHostnameCanonicalization"].as<std::string>() == "forward") {
            saslSSPIGlobalParams.canonicalization = HostnameCanonicalizationMode::kForward;
        } else if (params["security.sspiHostnameCanonicalization"].as<std::string>() ==
                   "forwardAndReverse") {
            saslSSPIGlobalParams.canonicalization =
                HostnameCanonicalizationMode::kForwardAndReverse;
        } else {
            return Status(ErrorCodes::InvalidOptions,
                          "Unrecognized sspiHostnameCanonicalization option");
        }
    }

    return Status::OK();
}

MONGO_STARTUP_OPTIONS_STORE(SASLSSPIOptions)(InitializerContext* context) {
    uassertStatusOK(storeSASLSSPIOptions(moe::startupOptionsParsed));
}

}  // namespace mongo

#endif  // ifdef _WIN32
