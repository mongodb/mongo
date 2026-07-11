// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/net/hostname_canonicalization.h"

#include <string>

namespace mongo {

class Status;

namespace optionenvironment {
class OptionSection;
class Environment;
}  // namespace optionenvironment

namespace moe = mongo::optionenvironment;

struct SASLSSPIGlobalParams {
    // HostnameCanonicalizationMode to use for resolving SASL hostname into the SPN's hostname
    HostnameCanonicalizationMode canonicalization = HostnameCanonicalizationMode::kNone;

    // Override the automatically detected realm
    std::string realmOverride;
};

extern SASLSSPIGlobalParams saslSSPIGlobalParams;

Status addSASLSSPIOptions(moe::OptionSection* options);
Status storeSASLSSPIOptions(const moe::Environment& params);

}  // namespace mongo
