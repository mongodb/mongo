/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/global_catalog/router_role_api/routing_table_cache_gossip_metadata_hook.h"

#include "mongo/db/global_catalog/router_role_api/gossiped_routing_cache_gen.h"
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
