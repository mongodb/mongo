// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <iosfwd>

namespace mongo {

/**
 * Outputs the version of MongoS as part of server startup.
 * Goes to `os` if nonnull, else to LOGV2.
 *
 * NOTE: Outputs the version of MongoS to `os` (as part of the --version option),
 * which reports different data than if `os` is null!
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void logMongosVersionInfo(std::ostream* os);

}  // namespace mongo
