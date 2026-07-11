// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/counter.h"
#include "mongo/util/modules.h"

namespace mongo::search_metrics {

extern Counter64& legacySearchQueryCount;

extern Counter64& extensionSearchQueryCount;

extern Counter64& onViewKickbackRetryCount;

extern Counter64& inUnionWithKickbackRetryCount;

extern Counter64& inLookupKickbackRetryCount;

extern Counter64& inHybridSearchKickbackRetryCount;

extern Counter64& inFacetKickbackRetryCount;

}  // namespace mongo::search_metrics
