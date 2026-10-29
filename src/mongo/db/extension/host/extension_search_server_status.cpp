// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/extension_search_server_status.h"

#include "mongo/db/commands/server_status/server_status_metric.h"

namespace mongo::search_metrics {

Counter64& legacySearchQueryCount = *MetricBuilder<Counter64>("extension.search.legacySearchUsed");

Counter64& extensionSearchQueryCount =
    *MetricBuilder<Counter64>("extension.search.extensionSearchUsed");

Counter64& onViewKickbackRetryCount =
    *MetricBuilder<Counter64>("extension.search.onViewKickbackRetries");

Counter64& inUnionWithKickbackRetryCount =
    *MetricBuilder<Counter64>("extension.search.inUnionWithKickbackRetries");

Counter64& inLookupKickbackRetryCount =
    *MetricBuilder<Counter64>("extension.search.inLookupKickbackRetries");

Counter64& inHybridSearchKickbackRetryCount =
    *MetricBuilder<Counter64>("extension.search.inHybridSearchKickbackRetries");

Counter64& inFacetKickbackRetryCount =
    *MetricBuilder<Counter64>("extension.search.inFacetKickbackRetries");

}  // namespace mongo::search_metrics
