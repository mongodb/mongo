// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/server_options.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/value.h"

#include <string>
#include <vector>

namespace mongo {

namespace optionenvironment {
class OptionSection;
class Environment;
}  // namespace optionenvironment

namespace moe = mongo::optionenvironment;

struct MongosGlobalParams {
    bool scriptingEnabled = true;  // Use "security.javascriptEnabled" to set this variable. Or use
                                   // --noscripting which will set it to false.

    // The config server connection string
    ConnectionString configdbs;
};

extern MongosGlobalParams mongosGlobalParams;

void printMongosHelp(const moe::OptionSection& options);

/**
 * Handle options that should come before validation, such as "help".
 *
 * Returns false if an option was found that implies we should prematurely exit with success.
 */
bool handlePreValidationMongosOptions(const moe::Environment& params,
                                      const std::vector<std::string>& args);

/**
 * Handle custom validation of mongos options that can not currently be done by using
 * Constraints in the Environment.  See the "validate" function in the Environment class for
 * more details.
 */
Status validateMongosOptions(const moe::Environment& params);

/**
 * Canonicalize mongos options for the given environment.
 *
 * For example, the options "noscripting" and "security.javascriptEnabled" and should all be merged
 * into "security.javascriptEnabled".
 */
Status canonicalizeMongosOptions(moe::Environment* params);

Status storeMongosOptions(const moe::Environment& params);
}  // namespace mongo
