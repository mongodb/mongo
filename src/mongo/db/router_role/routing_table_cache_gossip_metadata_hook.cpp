// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/router_role/routing_table_cache_gossip_metadata_hook.h"

#include "mongo/db/router_role/gossiped_routing_cache_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace rpc {

RoutingTableCacheGossipMetadataHook::RoutingTableCacheGossipMetadataHook(
    ServiceContext* serviceContext)
    : _serviceContext(serviceContext) {}

Status RoutingTableCacheGossipMetadataHook::writeRequestMetadata(OperationContext* opCtx,
                                                                 BSONObjBuilder* metadataBob) {
    return Status::OK();
}

Status RoutingTableCacheGossipMetadataHook::readReplyMetadata(OperationContext* opCtx,
                                                              const BSONObj& metadataObj) {
    try {
        const auto routerCacheVersionsObj =
            metadataObj[GenericReplyFields::kRoutingCacheGossipFieldName];
        if (!routerCacheVersionsObj.eoo()) {
            const auto catalogCache = Grid::get(_serviceContext)->catalogCache();
            for (const auto& elem : routerCacheVersionsObj.Array()) {
                const auto gossipedRoutingCache = GossipedRoutingCache::parse(
                    elem.Obj(), IDLParserContext("RoutingTableCacheGossipMetadataHook"));

                catalogCache->advanceCollectionTimeInStore(
                    gossipedRoutingCache.getNss(), gossipedRoutingCache.getCollectionVersion());
            }
        }
    } catch (...) {
        return exceptionToStatus();
    }

    return Status::OK();
}

}  // namespace rpc
}  // namespace mongo
