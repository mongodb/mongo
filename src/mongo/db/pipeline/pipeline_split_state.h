// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
namespace mongo {

/**
 * A SplitState specifies whether the pipeline is currently unsplit, split for the shards, or
 * split for merging.
 */
enum class [[MONGO_MOD_PUBLIC]] PipelineSplitState { kUnsplit, kSplitForShards, kSplitForMerge };

}  // namespace mongo
