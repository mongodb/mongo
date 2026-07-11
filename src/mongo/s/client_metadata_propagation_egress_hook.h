// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace rpc {

/**
 * Hook for attaching client, auth, and time-out metadata for requests made on behalf of a user.
 */
class [[MONGO_MOD_PUBLIC]] ClientMetadataPropagationEgressHook : public rpc::EgressMetadataHook {
public:
    ~ClientMetadataPropagationEgressHook() override = default;

    Status readReplyMetadata(OperationContext* opCtx, const BSONObj& metadataObj) final;
    Status writeRequestMetadata(OperationContext* opCtx, BSONObjBuilder* metadataBob) final;
};

}  // namespace rpc
}  // namespace mongo
