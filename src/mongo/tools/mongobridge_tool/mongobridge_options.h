// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace mongo {

namespace optionenvironment {
class OptionSection;
class Environment;
}  // namespace optionenvironment

namespace moe = mongo::optionenvironment;

struct MongoBridgeGlobalParams {
    int port = 0;
    std::int64_t seed = 0;
    std::string destUri;

    MongoBridgeGlobalParams() = default;
};

extern MongoBridgeGlobalParams mongoBridgeGlobalParams;

Status addMongoBridgeOptions(moe::OptionSection* options);

void printMongoBridgeHelp(std::ostream* out);

/**
 * Handle options that should come before validation, such as "help".
 *
 * Returns false if an option was found that implies we should prematurely exit with success.
 */
bool handlePreValidationMongoBridgeOptions(const moe::Environment& params);

Status storeMongoBridgeOptions(const moe::Environment& params,
                               const std::vector<std::string>& args);
}  // namespace mongo
