// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/idl/generic_argument_gen.h"
#include "mongo/util/version.h"

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
IFRSenderVersion makeLocalIFRSenderVersion();

}  // namespace mongo
