// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/extension_vector_search_server_status.h"

#include "mongo/db/commands/server_status/server_status_metric.h"

namespace mongo::vector_search_metrics {

Counter64& legacyVectorSearchQueryCount =
    *MetricBuilder<Counter64>("extension.vectorSearch.legacyVectorSearchUsed");

Counter64& extensionVectorSearchQueryCount =
    *MetricBuilder<Counter64>("extension.vectorSearch.extensionVectorSearchUsed");

Counter64& onViewKickbackRetryCount =
    *MetricBuilder<Counter64>("extension.vectorSearch.onViewKickbackRetries");

Counter64& inUnionWithKickbackRetryCount =
    *MetricBuilder<Counter64>("extension.vectorSearch.inUnionWithKickbackRetries");

Counter64& inLookupKickbackRetryCount =
    *MetricBuilder<Counter64>("extension.vectorSearch.inLookupKickbackRetries");

Counter64& inHybridSearchKickbackRetryCount =
    *MetricBuilder<Counter64>("extension.vectorSearch.inHybridSearchKickbackRetries");

}  // namespace mongo::vector_search_metrics
