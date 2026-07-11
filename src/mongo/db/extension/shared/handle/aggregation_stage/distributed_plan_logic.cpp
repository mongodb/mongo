// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/shared/handle/aggregation_stage/distributed_plan_logic.h"

#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/dpl_array_container.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"

namespace mongo::extension {

std::vector<VariantDPLHandle> DistributedPlanLogicAPI::extractShardsPipeline() {
    ::MongoExtensionDPLArrayContainer* container = nullptr;
    invokeCAndConvertStatusToException(
        [&]() { return _vtable().extract_shards_pipeline(get(), &container); });

    if (container == nullptr) {
        return {};
    }

    DPLArrayContainerHandle handle(container);
    return handle->transfer();
}

std::vector<VariantDPLHandle> DistributedPlanLogicAPI::extractMergingPipeline() {
    ::MongoExtensionDPLArrayContainer* container = nullptr;
    invokeCAndConvertStatusToException(
        [&]() { return _vtable().extract_merging_pipeline(get(), &container); });

    if (container == nullptr) {
        return {};
    }

    DPLArrayContainerHandle handle(container);
    return handle->transfer();
}

BSONObj DistributedPlanLogicAPI::getSortPattern() const {
    ::MongoExtensionByteBuf* buf = nullptr;
    invokeCAndConvertStatusToException([&]() { return _vtable().get_sort_pattern(get(), &buf); });

    if (buf == nullptr) {
        return BSONObj();
    }

    ExtensionByteBufHandle ownedBuf{buf};
    return bsonObjFromByteView(ownedBuf->getByteView()).getOwned();
}

}  // namespace mongo::extension
