// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {

/**
 * Given a QO and a QE pipeline, merge the pipelines' explain output providing the level of detail
 * specified by 'verbosity'.
 * Note: It's expected that the pipelines have the same number of stages.
 */
std::vector<Value> mergeExplains(const Pipeline& p1,
                                 const exec::agg::Pipeline& p2,
                                 const query_shape::SerializationOptions& opts);

/**
 * Utility to merge already generated explain outputs from two pipelines (QO and QE).
 */
std::vector<Value> mergeExplains(const std::vector<Value>& lhs, const std::vector<Value>& rhs);

}  // namespace mongo
