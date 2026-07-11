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
class Status;

namespace rpc {

/**
 * Data structure for storing a list of EgressMetadataHook.
 */
class [[MONGO_MOD_PUBLIC]] EgressMetadataHookList final : public EgressMetadataHook {
public:
    /**
     * Adds a hook to this list. The hooks are executed in the order they were added.
     */
    void addHook(std::unique_ptr<EgressMetadataHook>&& newHook);

    /**
     * Calls writeRequestMetadata on every hook in the order they were added. This will terminate
     * early if one of hooks returned a non OK status and return it. Note that metadataBob should
     * not be used if Status is not OK as the contents can be partial.
     */
    Status writeRequestMetadata(OperationContext* opCtx, BSONObjBuilder* metadataBob) override;

    /**
     * Calls readReplyMetadata on every hook in the order they were added. This will terminate
     * early if one of hooks returned a non OK status and return it. Note that metadataBob should
     * not be used if Status is not OK as the contents can be partial.
     */
    Status readReplyMetadata(OperationContext* opCtx, const BSONObj& metadataObj) override;

private:
    std::vector<std::unique_ptr<EgressMetadataHook>> _hooks;
};

}  // namespace rpc
}  // namespace mongo
