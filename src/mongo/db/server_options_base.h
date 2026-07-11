// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"

#include <string>

namespace mongo {
/**
 * Base server options that are available in all applications, standalone and embedded.
 *
 * Included by addGeneralServerOptions, don't call both.
 */
Status addBaseServerOptions(optionenvironment::OptionSection*);

/**
 * General server options for most standalone applications. Includes addBaseServerOptions.
 */
[[MONGO_MOD_PUBLIC]] Status addGeneralServerOptions(optionenvironment::OptionSection*);

Status validateSystemLogDestinationSetting(const std::string&);

Status validateSecurityClusterAuthModeSetting(const std::string&);

Status canonicalizeNetBindIpAll(optionenvironment::Environment*);

std::string getUnixDomainSocketFilePermissionsHelpText();

}  // namespace mongo
