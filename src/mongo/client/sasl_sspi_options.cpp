/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#ifdef _WIN32

#include "mongo/platform/basic.h"

#include "mongo/client/sasl_sspi_options.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

SASLSSPIGlobalParams saslSSPIGlobalParams;

Status addSASLSSPIOptions(moe::OptionSection* options) {
    moe::OptionSection sspiOptions("Kerberos Options");
    sspiOptions
        .addOptionChaining("security.sspiHostnameCanonicalization",
                           "sspiHostnameCanonicalization",
                           moe::String,
                           "DNS resolution strategy to use for hostname canonicalization. "
                           "May be one of: {none, forward, forwardAndReverse}")
        .setDefault(moe::Value(std::string("none")));
    sspiOptions
        .addOptionChaining("security.sspiRealmOverride",
                           "sspiRealmOverride",
                           moe::String,
                           "Override the detected realm with the provided string")
        .hidden();
    return options->addSection(sspiOptions);
}

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
    if (params.count("security.sspiRealmOverride")) {
        saslSSPIGlobalParams.realmOverride = params["security.sspiRealmOverride"].as<std::string>();
    }
    return Status::OK();
}

MONGO_MODULE_STARTUP_OPTIONS_REGISTER(SASLSSPIOptions)(InitializerContext* context) {
    return addSASLSSPIOptions(&moe::startupOptions);
}

MONGO_STARTUP_OPTIONS_STORE(SASLSSPIOptions)(InitializerContext* context) {
    return storeSASLSSPIOptions(moe::startupOptionsParsed);
}

}  // namespace mongo

#endif  // ifdef _WIN32
