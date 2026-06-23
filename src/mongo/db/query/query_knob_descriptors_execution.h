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
    /* End MONGO_EXPAND_QUERY_KNOBS_EXECUTION */
// clang-format on

namespace mongo::query_knobs {
DECLARE_QUERY_KNOBS(QueryExecutionKnobs, MONGO_EXPAND_QUERY_KNOBS_EXECUTION)
}  // namespace mongo::query_knobs
