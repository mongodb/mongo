// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace mongo {

/**
 * Handle custom validation of server options that can not currently be done by using
 * Constraints in the Environment.  See the "validate" function in the Environment class for
 * more details.
 */
[[MONGO_MOD_PUBLIC]] Status validateServerOptions(const optionenvironment::Environment& params);

/**
 * Canonicalize server options for the given environment.
 *
 * For example, the options "objcheck", "noobjcheck", and "net.wireObjectCheck" should all be
 * merged into "net.wireObjectCheck".
 */
[[MONGO_MOD_PUBLIC]] Status canonicalizeServerOptions(optionenvironment::Environment* params);

/**
 * Sets up the global server state necessary to be able to store the server options, based on how
 * the server was started.
 *
 * For example, saves the current working directory in serverGlobalParams.cwd so that relative paths
 * in server options can be interpreted correctly.
 */
[[MONGO_MOD_PUBLIC]] Status setupServerOptions(const std::vector<std::string>& args);

/**
 * Store the given parsed params in global server state.
 *
 * For example, sets the serverGlobalParams.port variable based on the net.port config parameter.
 */
[[MONGO_MOD_PUBLIC]] Status storeServerOptions(const optionenvironment::Environment& params);

}  // namespace mongo
