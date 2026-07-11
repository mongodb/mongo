// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mongo {

namespace optionenvironment {
class OptionSection;
class Environment;
}  // namespace optionenvironment

namespace moe = mongo::optionenvironment;

struct MongoCryptDGlobalParams {
    int port = 0;

    Seconds idleShutdownTimeout;

    MongoCryptDGlobalParams() = default;
};

extern MongoCryptDGlobalParams mongoCryptDGlobalParams;

Status addMongoCryptDOptions(moe::OptionSection* options);

void printMongoCryptDHelp(std::ostream* out);

/**
 * Handle options that should come before validation, such as "help".
 *
 * Returns false if an option was found that implies we should prematurely exit with success.
 */
bool handlePreValidationMongoCryptDOptions(const moe::Environment& params);

Status validateMongoCryptDOptions(const moe::Environment& params);
Status canonicalizeMongoCryptDOptions(moe::Environment* params);

Status storeMongoCryptDOptions(const moe::Environment& params,
                               const std::vector<std::string>& args);
}  // namespace mongo
