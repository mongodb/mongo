// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Represents the options for auto compaction.
 */
struct [[MONGO_MOD_PUBLIC]] AutoCompactOptions {
    // Toggle to enable/disable the service.
    bool enable = false;
    // Whether background compaction should run once on the database and stop.
    bool runOnce = false;
    // Minimum amount of MB to reclaim for compaction to proceed.
    boost::optional<int64_t> freeSpaceTargetMB;
    // Idents that are skipped by background compaction.
    std::vector<std::string_view> excludedIdents;
};

/**
 * Represents the options for compaction.
 */
struct [[MONGO_MOD_PUBLIC]] CompactOptions {
    // When enabled, estimate the work compaction can do.
    bool dryRun = false;
    // Minimum amount of MB to reclaim for compaction to proceed.
    boost::optional<int64_t> freeSpaceTargetMB;
};

}  // namespace mongo
