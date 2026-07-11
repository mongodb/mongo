// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/server_parameter.h"
#include "mongo/util/modules.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/value.h"

#include <map>
#include <string>
#include <vector>

namespace mongo {

namespace moe = mongo::optionenvironment;

namespace server_options_detail {
StatusWith<BSONObj> applySetParameterOptions(const std::map<std::string, std::string>& toApply,
                                             ServerParameterSet& parameterSet);
}  // namespace server_options_detail

/**
 * Handle custom validation of base options that can not currently be done by using
 * Constraints in the Environment.  See the "validate" function in the Environment class for
 * more details.
 */
Status validateBaseOptions(const moe::Environment& params);

/**
 * Canonicalize base options for the given environment.
 *
 * For example, the options "objcheck", "noobjcheck", and "net.wireObjectCheck" should all be
 * merged into "net.wireObjectCheck".
 */
Status canonicalizeBaseOptions(moe::Environment* params);

/**
 * Sets up the global server state necessary to be able to store the server options, based on how
 * the server was started.
 *
 * For example, saves the current working directory in serverGlobalParams.cwd so that relative paths
 * in server options can be interpreted correctly.
 */
Status setupBaseOptions(const std::vector<std::string>& args);

/**
 * Store the given parsed params in global server state.
 *
 * For example, sets the serverGlobalParams.quiet variable based on the systemLog.quiet config
 * parameter.
 */
Status storeBaseOptions(const moe::Environment& params);

}  // namespace mongo
