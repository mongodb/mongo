// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/shard_role/shard_role.h"

namespace mongo {

// These definitions live in the `pipeline` library because they reference
// `CollectionOrViewAcquisition` and `CollectionRoutingInfo` - having LPDS depend on `shard_role`
// would introduce a cycle. Note that the header forward-declares both types so LPDS-only consumers
// don't transitively pull `shard_role.h` / `catalog_cache.h`.

void LiteParsedPipeline::validateWithCollectionMetadata(
    const CollectionOrViewAcquisition& collOrView) const {
    if (collOrView.collectionExists() && collOrView.getCollectionPtr()->isTimeseriesCollection()) {
        validateTimeseries();
    }
}

void LiteParsedPipeline::validateWithCollectionMetadata(const CollectionRoutingInfo& cri) const {
    if (cri.hasRoutingTable() && cri.getChunkManager().isTimeseriesCollection()) {
        validateTimeseries();
    }
}

}  // namespace mongo
