// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * EXPAND table of QueryKnob<T>s mirroring the server parameters in query_execution_knobs.idl.
 */

#pragma once

#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_knobs/query_knob.h"

// clang-format off
#define MONGO_EXPAND_QUERY_KNOBS_EXECUTION(KNOB)                                          \
    KNOB(kSbeDisableGroupPushdown,                                                        \
         kInternalQuerySlotBasedExecutionDisableGroupPushdownName,                        \
         internalQuerySlotBasedExecutionDisableGroupPushdown,                             \
         getSbeDisableGroupPushdownForOp)                                                 \
    KNOB(kSbeDisableLookupPushdown,                                                       \
         kInternalQuerySlotBasedExecutionDisableLookupPushdownName,                       \
         internalQuerySlotBasedExecutionDisableLookupPushdown,                            \
         getSbeDisableLookupPushdownForOp)                                                \
    KNOB(kSbeDisableLookupUnwindPushdown,                                                 \
         kInternalQuerySlotBasedExecutionDisableLookupUnwindPushdownName,                 \
         internalQuerySlotBasedExecutionDisableLookupUnwindPushdown,                      \
         getSbeDisableLookupUnwindPushdownForOp)                                          \
    KNOB(kSbeDisableTimeSeriesPushdown,                                                   \
         kInternalQuerySlotBasedExecutionDisableTimeSeriesPushdownName,                   \
         internalQuerySlotBasedExecutionDisableTimeSeriesPushdown,                        \
         getSbeDisableTimeSeriesForOp)                                                    \
    KNOB(kMeasureQueryExecutionTimeInNanoseconds,                                         \
         kInternalMeasureQueryExecutionTimeInNanosecondsName,                             \
         internalMeasureQueryExecutionTimeInNanoseconds,                                  \
         getMeasureQueryExecutionTimeInNanoseconds)                                       \
    KNOB(kSpillingMinAvailableDiskSpaceBytes,                                             \
         kInternalQuerySpillingMinAvailableDiskSpaceBytesName,                            \
         internalQuerySpillingMinAvailableDiskSpaceBytes,                                 \
         getInternalQuerySpillingMinAvailableDiskSpaceBytes)                              \
    KNOB(kMaxGroupAccumulatorsInSbe,                                                      \
         kInternalMaxGroupAccumulatorsInSbeName,                                          \
         gInternalMaxGroupAccumulatorsInSbe,                                              \
         getMaxGroupAccumulatorsInSbe)                                                    \
    KNOB(kQueryFrameworkControl,                                                          \
         kInternalQueryFrameworkControlName,                                              \
         QueryFrameworkControl,                                                           \
         getInternalQueryFrameworkControlForOp)                                           \
    KNOB(kSbeHashAggIncreasedSpillingMode,                                                \
         kInternalQuerySlotBasedExecutionHashAggIncreasedSpillingName,                    \
         SbeHashAggIncreasedSpillingMode,                                                 \
         getSbeHashAggIncreasedSpillingMode)                                              \
    KNOB(kOperationResponseMaxMS,                                                         \
         kInternalOperationResponseMaxMSName,                                             \
         internalOperationResponseMaxMS,                                                  \
         getOperationResponseMaxMS)                                                       \
    KNOB(kChangeStreamRespectsReadPreference,                                             \
         kInternalChangeStreamRespectsReadPreferenceName,                                 \
         internalChangeStreamRespectsReadPreference,                                      \
         getChangeStreamRespectsReadPreference)                                           \
    KNOB(kDisableLookupExecutionUsingHashJoin,                                            \
         kInternalQueryDisableLookupExecutionUsingHashJoinName,                           \
         internalQueryDisableLookupExecutionUsingHashJoin,                                \
         getDisableLookupExecutionUsingHashJoin)                                          \
    KNOB(kCollectionMaxDataSizeBytesToChooseHashJoin,                                     \
         kInternalQueryCollectionMaxDataSizeBytesToChooseHashJoinName,                    \
         internalQueryCollectionMaxDataSizeBytesToChooseHashJoin,                         \
         getCollectionMaxDataSizeBytesToChooseHashJoin)                                   \
    KNOB(kCollectionMaxNoOfDocumentsToChooseHashJoin,                                     \
         kInternalQueryCollectionMaxNoOfDocumentsToChooseHashJoinName,                    \
         internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin,                         \
         getCollectionMaxNoOfDocumentsToChooseHashJoin)                                   \
    KNOB(kCollectionMaxStorageSizeBytesToChooseHashJoin,                                  \
         kInternalQueryCollectionMaxStorageSizeBytesToChooseHashJoinName,                 \
         internalQueryCollectionMaxStorageSizeBytesToChooseHashJoin,                      \
         getCollectionMaxStorageSizeBytesToChooseHashJoin)                                \
    KNOB(kDisableSingleFieldExpressExecutor,                                              \
         kInternalQueryDisableSingleFieldExpressExecutorName,                             \
         internalQueryDisableSingleFieldExpressExecutor,                                  \
         getDisableSingleFieldExpressExecutor)                                            \
    KNOB(kChangeStreamUpdateLookupMaxBatchSize,                                           \
         kInternalChangeStreamUpdateLookupMaxBatchSizeName,                               \
         internalChangeStreamUpdateLookupMaxBatchSize,                                    \
         getChangeStreamUpdateLookupMaxBatchSize)                                         \
    KNOB(kChangeStreamUpdateLookupMaxInputBytes,                                          \
         kInternalChangeStreamUpdateLookupMaxInputBytesName,                              \
         internalChangeStreamUpdateLookupMaxInputBytes,                                   \
         getChangeStreamUpdateLookupMaxInputBytes)                                        \
    KNOB(kChangeStreamUpdateLookupMaxOutputBytes,                                         \
         kInternalChangeStreamUpdateLookupMaxOutputBytesName,                             \
         internalChangeStreamUpdateLookupMaxOutputBytes,                                  \
         getChangeStreamUpdateLookupMaxOutputBytes)                                       \
    KNOB(kMaxPushBytes,                                                                   \
         kInternalQueryMaxPushBytesName,                                                  \
         internalQueryMaxPushBytes,                                                       \
         getMaxPushBytes)                                                                 \
    KNOB(kMaxAddToSetBytes,                                                               \
         kInternalQueryMaxAddToSetBytesName,                                              \
         internalQueryMaxAddToSetBytes,                                                   \
         getMaxAddToSetBytes)                                                             \
    KNOB(kMaxConcatArraysBytes,                                                           \
         kInternalQueryMaxConcatArraysBytesName,                                          \
         internalQueryMaxConcatArraysBytes,                                               \
         getMaxConcatArraysBytes)                                                         \
    KNOB(kMaxSetUnionBytes,                                                               \
         kInternalQueryMaxSetUnionBytesName,                                              \
         internalQueryMaxSetUnionBytes,                                                   \
         getMaxSetUnionBytes)                                                             \
    KNOB(kTopNAccumulatorBytes,                                                           \
         kInternalQueryTopNAccumulatorBytesName,                                          \
         internalQueryTopNAccumulatorBytes,                                               \
         getTopNAccumulatorBytes)                                                         \
    KNOB(kMaxPercentileAccumulatorBytes,                                                  \
         kInternalQueryMaxPercentileAccumulatorBytesName,                                 \
         internalQueryMaxPercentileAccumulatorBytes,                                      \
         getMaxPercentileAccumulatorBytes)                                                \
    KNOB(kMaxSingleExpressionMemoryUsageBytes,                                            \
         kInternalQueryMaxSingleExpressionMemoryUsageBytesName,                           \
         internalQueryMaxSingleExpressionMemoryUsageBytes,                                \
         getMaxSingleExpressionMemoryUsageBytes)                                          \
    KNOB(kMaxMemoryUsageBytesPerOperation,                                                \
         kInternalQueryMaxMemoryUsageBytesPerOperationName,                               \
         internalQueryMaxMemoryUsageBytesPerOperation,                                    \
         getMaxMemoryUsageBytesPerOperation)                                              \
    /* End MONGO_EXPAND_QUERY_KNOBS_EXECUTION */
// clang-format on

namespace mongo::query_knobs {
DECLARE_QUERY_KNOBS(QueryExecutionKnobs, MONGO_EXPAND_QUERY_KNOBS_EXECUTION)
}  // namespace mongo::query_knobs
