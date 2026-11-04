// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_knob_descriptors_join_optimization.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/query/query_knobs/query_knob_registry.h"

namespace mongo::query_knobs {
REGISTER_QUERY_KNOBS(QueryJoinOptimizationKnobs, MONGO_EXPAND_QUERY_KNOBS_JOIN_OPTIMIZATION)
}  // namespace mongo::query_knobs
