// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/timeseries/timeseries_translation.h"
#include "mongo/util/modules.h"

namespace mongo {

namespace aggregation_hint_translation {

/**
 * Translates the index hint in the aggregation request if it exists and must be translated. The
 * only supported translation is for viewless timeseries collections.
 *
 * Viewless timeseries translations require accurate collection data from the global or shard
 * catalog. It is the caller's responsibility to ensure the catalog is accurate.
 */
template <class T>
inline void translateIndexHintIfRequired(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         const T& catalogData,
                                         AggregateCommandRequest& request) {
    timeseries::translateIndexHintIfRequired(expCtx, catalogData, request);
}
}  // namespace aggregation_hint_translation

}  // namespace mongo
