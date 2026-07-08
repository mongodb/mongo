/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
    KNOB(kSingleDocumentTransformationStageMaxExpressionEvaluationBytes,                  \
         kInternalSingleDocumentTransformationStageMaxExpressionEvaluationBytesName,      \
         internalSingleDocumentTransformationStageMaxExpressionEvaluationBytes,           \
         getSingleDocumentTransformationStageMaxExpressionEvaluationBytes)                \
    KNOB(kMatchStageMaxExpressionEvaluationBytes,                                         \
         kInternalMatchStageMaxExpressionEvaluationBytesName,                             \
         internalMatchStageMaxExpressionEvaluationBytes,                                  \
         getMatchStageMaxExpressionEvaluationBytes)                                       \
    KNOB(kLookupStageMaxExpressionEvaluationBytes,                                        \
         kInternalLookupStageMaxExpressionEvaluationBytesName,                            \
         internalLookupStageMaxExpressionEvaluationBytes,                                 \
         getLookupStageMaxExpressionEvaluationBytes)                                      \
    KNOB(kRedactStageMaxExpressionEvaluationBytes,                                        \
         kInternalRedactStageMaxExpressionEvaluationBytesName,                            \
         internalRedactStageMaxExpressionEvaluationBytes,                                 \
         getRedactStageMaxExpressionEvaluationBytes)                                      \
    /* End MONGO_EXPAND_QUERY_KNOBS_STAGE_MEMORY_LIMIT */
// clang-format on

namespace mongo::query_knobs {
DECLARE_QUERY_KNOBS(QueryStageMemoryLimitKnobs, MONGO_EXPAND_QUERY_KNOBS_STAGE_MEMORY_LIMIT)
}  // namespace mongo::query_knobs
