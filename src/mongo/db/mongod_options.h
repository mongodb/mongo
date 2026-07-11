// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"

#include <string>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {

namespace optionenvironment {
class OptionSection;
class Environment;
}  // namespace optionenvironment

namespace moe = mongo::optionenvironment;


Status addMongodOptions(moe::OptionSection* options);

void printMongodHelp(const moe::OptionSection& options);

/**
 * Handle options that should come before validation, such as "help".
 *
 * Returns false if an option was found that implies we should prematurely exit with success.
 */
bool handlePreValidationMongodOptions(const moe::Environment& params,
                                      const std::vector<std::string>& args);

/**
 * Handle custom validation of mongod options that can not currently be done by using
 * Constraints in the Environment.  See the "validate" function in the Environment class for
 * more details.
 */
Status validateMongodOptions(const moe::Environment& params);

/**
 * Canonicalize mongod options for the given environment.
 *
 * For example, "nounixsocket" maps to "net.unixDomainSocket.enabled".
 */
Status canonicalizeMongodOptions(moe::Environment* params);

Status storeMongodOptions(const moe::Environment& params);

}  // namespace mongo
