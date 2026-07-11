// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <boost/filesystem.hpp>

namespace mongo {

/**
 * Checks the available disk space to ensure there is a minimum amount required for spilling.
 *
 * TODO SERVER-113196 Determine if this can be made private to some module.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status ensureSufficientDiskSpaceForSpilling(
    const boost::filesystem::path& path, int64_t minRequired);

}  // namespace mongo
