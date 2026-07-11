// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/util/modules.h"

#include <memory>
#include <vector>

namespace mongo {

class BSONObj;
class BSONObjBuilder;
struct HostAndPort;
class OperationContext;
class ServiceContext;
class Status;

namespace rpc {

class [[MONGO_MOD_NEEDS_REPLACEMENT]] VectorClockMetadataHook : public EgressMetadataHook {
public:
    explicit VectorClockMetadataHook(ServiceContext* service);

    Status writeRequestMetadata(OperationContext* opCtx, BSONObjBuilder* metadataBob) override;

    Status readReplyMetadata(OperationContext* opCtx, const BSONObj& metadataObj) override;

private:
    ServiceContext* _service;
};

}  // namespace rpc
}  // namespace mongo
