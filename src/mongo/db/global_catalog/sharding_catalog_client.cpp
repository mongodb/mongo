// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/sharding_catalog_client.h"

namespace mongo {

WriteConcernOptions ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter() {
    return WriteConcernOptions{1, WriteConcernOptions::SyncMode::UNSET, Seconds(0)};
}


// An empty namespace is used as a reserved value to persist initialization metadata of
// config.placementHistory.
const NamespaceString ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker{};

}  // namespace mongo
