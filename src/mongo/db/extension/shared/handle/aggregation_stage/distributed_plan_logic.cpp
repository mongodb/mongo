/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/extension/shared/handle/aggregation_stage/distributed_plan_logic.h"

#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/dpl_array_container.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"

namespace mongo::extension {

std::vector<VariantDPLHandle> DistributedPlanLogicHandle::getShardsPipeline() const {
    assertValid();
    ::MongoExtensionDPLArrayContainer* container = nullptr;
    invokeCAndConvertStatusToException(
        [&]() { return vtable().get_shards_pipeline(get(), &container); });

    if (container == nullptr) {
        return {};
    }

    DPLArrayContainerHandle handle(container);
    return handle.transfer();
}

std::vector<VariantDPLHandle> DistributedPlanLogicHandle::getMergingPipeline() const {
    assertValid();
    ::MongoExtensionDPLArrayContainer* container = nullptr;
    invokeCAndConvertStatusToException(
        [&]() { return vtable().get_merging_pipeline(get(), &container); });

    if (container == nullptr) {
        return {};
    }

    DPLArrayContainerHandle handle(container);
    return handle.transfer();
}

BSONObj DistributedPlanLogicHandle::getSortPattern() const {
    assertValid();
    ::MongoExtensionByteBuf* buf = nullptr;
    invokeCAndConvertStatusToException([&]() { return vtable().get_sort_pattern(get(), &buf); });

    if (buf == nullptr) {
        return BSONObj();
    }

    ExtensionByteBufHandle ownedBuf{buf};
    return bsonObjFromByteView(ownedBuf.getByteView()).getOwned();
}

}  // namespace mongo::extension
