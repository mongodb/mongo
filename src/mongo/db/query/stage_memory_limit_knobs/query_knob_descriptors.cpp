// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_memory_limit_knobs/query_knob_descriptors.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/query/query_knobs/query_knob_registry.h"

namespace mongo::query_knobs {
REGISTER_QUERY_KNOBS(QueryStageMemoryLimitKnobs, MONGO_EXPAND_QUERY_KNOBS_STAGE_MEMORY_LIMIT)
}  // namespace mongo::query_knobs
