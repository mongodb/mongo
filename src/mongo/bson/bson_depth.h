// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstdint>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
/**
 * Controls the maximum BSON depth tolerated by the server.
 */
struct BSONDepth {
    // The default BSON depth nesting limit.
    static constexpr std::int32_t kDefaultMaxAllowableDepth = 200;

    // The number of extra levels of nesting above the storage depth limit that the server will
    // tolerate.
    static constexpr std::uint32_t kExtraSystemDepthLevels = 20;

    // The minimum allowable value for the BSON depth parameter. Choose a value such that the max
    // depth for user storage will be at least 1.
    static constexpr std::int32_t kBSONDepthParameterFloor = kExtraSystemDepthLevels + 1;

    // The maximum allowable value for the BSON depth parameter.
    static constexpr std::int32_t kBSONDepthParameterCeiling = 250;

    // The depth of BSON accepted by the server. Configurable via the 'maxBSONDepth' server
    // parameter.
    static std::int32_t maxAllowableDepth;

    /**
     * Returns the maximum allowable BSON depth as an unsigned integer. Note that this is a hard
     * limit -- any BSON document that exceeds this limit should be considered invalid.
     */
    static std::uint32_t getMaxAllowableDepth();

    /**
     * Returns the BSON nesting depth limit for stored objects. User documents that exceed this
     * depth are not valid for storage. This limit is slightly lower than the hard limit in
     * getMaxAllowableDepth(), since we may generate things like oplog entries from these documents
     * that contain extra levels of nesting.
     */
    static std::uint32_t getMaxDepthForUserStorage();
};
using BSONDepthIndex = std::uint8_t;
}  // namespace mongo
