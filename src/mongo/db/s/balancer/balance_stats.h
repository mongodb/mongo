// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mongo {

class ChunkManager;
class ZoneInfo;

/**
 * Returns the maximum chunk imbalance (most chunks in shard minus least chunks in shard) among each
 * zone. The default unlabeled zone is considered as its own zone.
 */
int64_t getMaxChunkImbalanceCount(const ChunkManager& routingInfo,
                                  const std::vector<ShardType>& allShards,
                                  const ZoneInfo& zoneInfo);

}  // namespace mongo
