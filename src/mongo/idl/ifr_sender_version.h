// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/idl/generic_argument_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/version.h"

#include <string>

namespace mongo {

/**
 * Builds an 'IFRSenderVersion' from a 'VersionInfoInterface', decomposing the running binary's
 * version into its (major, minor, patch, extra) components. The 'extra' component encodes
 * pre-release ordering, so comparing the result against another 'IFRSenderVersion' yields correct
 * version ordering (see the IFRSenderVersion definition in generic_argument.idl).
 */
IFRSenderVersion makeIFRSenderVersion(const VersionInfoInterface& provider);

/**
 * Convenience overload that uses the process-wide 'VersionInfoInterface::instance()'.
 */
[[MONGO_MOD_PUBLIC]]
IFRSenderVersion makeLocalIFRSenderVersion();

/**
 * Renders an 'IFRSenderVersion' for diagnostics as "major.minor.patch", appending the pre-release
 * 'extra' component only when set (a release candidate has extra < 0, which already carries its own
 * sign, e.g. "9.0.0-23") so rc builds stay legible.
 */
[[MONGO_MOD_PUBLIC]]
std::string formatSenderVersion(const IFRSenderVersion& v);

}  // namespace mongo
