// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/counter.h"
#include "mongo/util/modules.h"

/**
 * Counters for vector search behavior and extension $vectorSearch usage. Used by server status
 * and metrics; extensionVectorSearchQueryCount is incremented when an extension $vectorSearch
 * stage is executed.
 */
namespace mongo::vector_search_metrics {

extern Counter64& legacyVectorSearchQueryCount;

extern Counter64& extensionVectorSearchQueryCount;

extern Counter64& onViewKickbackRetryCount;

extern Counter64& inUnionWithKickbackRetryCount;

extern Counter64& inLookupKickbackRetryCount;

extern Counter64& inHybridSearchKickbackRetryCount;

}  // namespace mongo::vector_search_metrics
