// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bson_depth.h"

namespace mongo {
constexpr std::int32_t BSONDepth::kDefaultMaxAllowableDepth;
constexpr std::int32_t BSONDepth::kBSONDepthParameterFloor;
constexpr std::int32_t BSONDepth::kBSONDepthParameterCeiling;

std::int32_t BSONDepth::maxAllowableDepth = BSONDepth::kDefaultMaxAllowableDepth;

std::uint32_t BSONDepth::getMaxAllowableDepth() {
    return static_cast<std::uint32_t>(BSONDepth::maxAllowableDepth);
}

std::uint32_t BSONDepth::getMaxDepthForUserStorage() {
    return static_cast<std::uint32_t>(BSONDepth::maxAllowableDepth -
                                      BSONDepth::kExtraSystemDepthLevels);
}
}  // namespace mongo
