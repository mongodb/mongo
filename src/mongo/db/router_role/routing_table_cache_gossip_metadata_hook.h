// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/util/modules.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
class OperationContext;
class ServiceContext;
class Status;

namespace rpc {

/**
 * Metadata hook that gossips-in sharding collection routing table versions from the metadata
 * reported on shard responses.
 */
class [[MONGO_MOD_PARENT_PRIVATE]] RoutingTableCacheGossipMetadataHook : public EgressMetadataHook {
public:
    RoutingTableCacheGossipMetadataHook(ServiceContext* serviceContext);
    ~RoutingTableCacheGossipMetadataHook() override = default;

    Status writeRequestMetadata(OperationContext* opCtx, BSONObjBuilder* metadataBob) final;

    Status readReplyMetadata(OperationContext* opCtx, const BSONObj& metadataObj) final;

private:
    ServiceContext* _serviceContext;
};

}  // namespace rpc
}  // namespace mongo
