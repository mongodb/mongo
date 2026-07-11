// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * EXPAND table of QueryKnob<T>s mirroring the server parameters in
 * stage_memory_limit_knobs.idl.
 */

#pragma once

#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/db/query/stage_memory_limit_knobs/stage_memory_limit_knobs_gen.h"

// clang-format off
#define MONGO_EXPAND_QUERY_KNOBS_STAGE_MEMORY_LIMIT(KNOB)                                 \
    KNOB(kDocumentSourceLookupCacheSizeBytes,                                             \
         kInternalDocumentSourceLookupCacheSizeBytesName,                                 \
         internalDocumentSourceLookupCacheSizeBytes,                                      \
         getDocumentSourceLookupCacheSizeBytes)                                           \
    KNOB(kDocumentSourceGraphLookupMaxMemoryBytes,                                        \
         kInternalDocumentSourceGraphLookupMaxMemoryBytesName,                            \
         internalDocumentSourceGraphLookupMaxMemoryBytes,                                 \
         getDocumentSourceGraphLookupMaxMemoryBytes)                                      \
    KNOB(kDocumentSourceGroupMaxMemoryBytes,                                              \
         kInternalDocumentSourceGroupMaxMemoryBytesName,                                  \
         internalDocumentSourceGroupMaxMemoryBytes,                                       \
         getDocumentSourceGroupMaxMemoryBytes)                                            \
    KNOB(kDocumentSourceSetWindowFieldsMaxMemoryBytes,                                    \
         kInternalDocumentSourceSetWindowFieldsMaxMemoryBytesName,                        \
         internalDocumentSourceSetWindowFieldsMaxMemoryBytes,                             \
         getDocumentSourceSetWindowFieldsMaxMemoryBytes)                                  \
    KNOB(kDocumentSourceBucketAutoMaxMemoryBytes,                                         \
         kInternalDocumentSourceBucketAutoMaxMemoryBytesName,                             \
         internalDocumentSourceBucketAutoMaxMemoryBytes,                                  \
         getDocumentSourceBucketAutoMaxMemoryBytes)                                       \
    KNOB(kDocumentSourceDensifyMaxMemoryBytes,                                            \
         kInternalDocumentSourceDensifyMaxMemoryBytesName,                                \
         internalDocumentSourceDensifyMaxMemoryBytes,                                     \
         getDocumentSourceDensifyMaxMemoryBytes)                                          \
    KNOB(kQueryFacetBufferSizeBytes,                                                      \
         kInternalQueryFacetBufferSizeBytesName,                                          \
         internalQueryFacetBufferSizeBytes,                                               \
         getQueryFacetBufferSizeBytes)                                                    \
    KNOB(kTextOrStageMaxMemoryBytes,                                                      \
         kInternalTextOrStageMaxMemoryBytesName,                                          \
         internalTextOrStageMaxMemoryBytes,                                               \
         getTextOrStageMaxMemoryBytes)                                                    \
    KNOB(kSbeHashLookupApproxMemoryUseInBytesBeforeSpill,                                 \
         kInternalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpillName, \
         internalQuerySBELookupApproxMemoryUseInBytesBeforeSpill,                         \
         getSbeHashLookupApproxMemoryUseInBytesBeforeSpill)                               \
    KNOB(kSbeHashAggApproxMemoryUseInBytesBeforeSpill,                                    \
         kInternalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpillName,    \
         internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill,                            \
         getSbeHashAggApproxMemoryUseInBytesBeforeSpill)                                  \
    KNOB(kQueryMaxSpoolMemoryUsageBytes,                                                  \
         kInternalQueryMaxSpoolMemoryUsageBytesName,                                      \
         internalQueryMaxSpoolMemoryUsageBytes,                                           \
         getQueryMaxSpoolMemoryUsageBytes)                                                \
    KNOB(kQueryMaxBlockingSortMemoryUsageBytes,                                           \
         kInternalQueryMaxBlockingSortMemoryUsageBytesName,                               \
         internalQueryMaxBlockingSortMemoryUsageBytes,                                    \
         getQueryMaxBlockingSortMemoryUsageBytes)                                         \
    KNOB(kOrStageMaxMemoryBytes,                                                          \
         kInternalOrStageMaxMemoryBytesName,                                              \
         internalOrStageMaxMemoryBytes,                                                   \
         getOrStageMaxMemoryBytes)                                                        \
    KNOB(kNearStageMaxMemoryBytes,                                                        \
         kInternalNearStageMaxMemoryBytesName,                                            \
         internalNearStageMaxMemoryBytes,                                                 \
         getNearStageMaxMemoryBytes)                                                      \
    KNOB(kMergeSortStageMaxMemoryBytes,                                                   \
         kInternalMergeSortStageMaxMemoryBytesName,                                       \
         internalMergeSortStageMaxMemoryBytes,                                            \
         getMergeSortStageMaxMemoryBytes)                                                 \
    KNOB(kIndexScanStageMaxMemoryBytes,                                                   \
         kInternalIndexScanStageMaxMemoryBytesName,                                       \
         internalIndexScanStageMaxMemoryBytes,                                            \
         getIndexScanStageMaxMemoryBytes)                                                 \
    KNOB(kSbeUniqueStageMaxMemoryBytes,                                                   \
         kInternalSlotBasedExecutionUniqueStageMaxMemoryBytesName,                        \
         internalSBEUniqueStageMaxMemoryBytes,                                            \
         getSbeUniqueStageMaxMemoryBytes)                                                 \
    KNOB(kSbeMergeJoinStageMaxMemoryBytes,                                                \
         kInternalSlotBasedExecutionMergeJoinStageMaxMemoryBytesName,                     \
         internalSBEMergeJoinStageMaxMemoryBytes,                                         \
         getSbeMergeJoinStageMaxMemoryBytes)                                              \
    KNOB(kSbeAndHashStageMaxMemoryBytes,                                                  \
         kInternalSlotBasedExecutionAndHashStageMaxMemoryBytesName,                       \
         internalSBEAndHashStageMaxMemoryBytes,                                           \
         getSbeAndHashStageMaxMemoryBytes)                                                \
    KNOB(kSbeHashJoinApproxMemoryUseInBytesBeforeSpill,                                   \
         kInternalQuerySlotBasedExecutionHashJoinApproxMemoryUseInBytesBeforeSpillName,   \
         internalQuerySBEHashJoinApproxMemoryUseInBytesBeforeSpill,                       \
         getSbeHashJoinApproxMemoryUseInBytesBeforeSpill)                                 \
    KNOB(kUpdateStageMaxMemoryBytes,                                                      \
         kInternalUpdateStageMaxMemoryBytesName,                                          \
         internalUpdateStageMaxMemoryBytes,                                               \
         getUpdateStageMaxMemoryBytes)                                                    \
    KNOB(kCountScanStageMaxMemoryBytes,                                                   \
         kInternalCountScanStageMaxMemoryBytesName,                                       \
         internalCountScanStageMaxMemoryBytes,                                            \
         getCountScanStageMaxMemoryBytes)                                                 \
    /* End MONGO_EXPAND_QUERY_KNOBS_STAGE_MEMORY_LIMIT */
// clang-format on

namespace mongo::query_knobs {
DECLARE_QUERY_KNOBS(QueryStageMemoryLimitKnobs, MONGO_EXPAND_QUERY_KNOBS_STAGE_MEMORY_LIMIT)
}  // namespace mongo::query_knobs
