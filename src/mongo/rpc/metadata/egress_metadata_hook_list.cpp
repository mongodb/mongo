// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/metadata/egress_metadata_hook_list.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

#include <utility>

namespace mongo {
namespace rpc {

void EgressMetadataHookList::addHook(std::unique_ptr<EgressMetadataHook>&& newHook) {
    _hooks.emplace_back(std::forward<std::unique_ptr<EgressMetadataHook>>(newHook));
}

Status EgressMetadataHookList::writeRequestMetadata(OperationContext* opCtx,
                                                    BSONObjBuilder* metadataBob) {
    for (auto&& hook : _hooks) {
        auto status = hook->writeRequestMetadata(opCtx, metadataBob);
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status EgressMetadataHookList::readReplyMetadata(OperationContext* opCtx,
                                                 const BSONObj& metadataObj) {
    for (auto&& hook : _hooks) {
        auto status = hook->readReplyMetadata(opCtx, metadataObj);
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

}  // namespace rpc
}  // namespace mongo
