// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/memory_tracking/memory_usage_limit.h"
#include "mongo/util/modules.h"

namespace mongo {

enum class StageMemoryLimit {
    DocumentSourceLookupCacheSizeBytes,
    DocumentSourceGraphLookupMaxMemoryBytes,
    DocumentSourceGroupMaxMemoryBytes,
    DocumentSourceSetWindowFieldsMaxMemoryBytes,
    DocumentSourceBucketAutoMaxMemoryBytes,
    DocumentSourceDensifyMaxMemoryBytes,
    QueryFacetBufferSizeBytes,
    TextOrStageMaxMemoryBytes,
    QuerySBELookupApproxMemoryUseInBytesBeforeSpill,
    QuerySBEAggApproxMemoryUseInBytesBeforeSpill,
    QueryMaxSpoolMemoryUsageBytes,
    QueryMaxBlockingSortMemoryUsageBytes,
    OrStageMaxMemoryBytes,
    NearStageMaxMemoryBytes,
    MergeSortStageMaxMemoryBytes,
    IndexScanStageMaxMemoryBytes,
    SBEUniqueStageMaxMemoryBytes,
    SBEMergeJoinStageMaxMemoryBytes,
    SBEAndHashStageMaxMemoryBytes,
    QuerySBEHashJoinApproxMemoryUseInBytesBeforeSpill,
    UpdateStageMaxMemoryBytes,
    CountScanStageMaxMemoryBytes,
};

/**
 * Returns the memory limit for the given stage according to the server parameters. Call 'get()' on
 * the result for a plain byte count.
 */
MemoryUsageLimit loadMemoryLimit(StageMemoryLimit stage);

}  // namespace mongo
