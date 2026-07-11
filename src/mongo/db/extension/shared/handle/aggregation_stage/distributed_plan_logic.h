// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/modules.h"

namespace mongo::extension {

class DistributedPlanLogicAPI;

template <>
struct c_api_to_cpp_api<::MongoExtensionDistributedPlanLogic> {
    using CppApi_t = DistributedPlanLogicAPI;
};

/**
 * DistributedPlanLogicHandle is a wrapper around a MongoExtensionDistributedPlanLogic vtable API.
 */
class DistributedPlanLogicAPI : public VTableAPI<::MongoExtensionDistributedPlanLogic> {
public:
    DistributedPlanLogicAPI(::MongoExtensionDistributedPlanLogic* dpl)
        : VTableAPI<::MongoExtensionDistributedPlanLogic>(dpl) {}

    std::vector<VariantDPLHandle> extractShardsPipeline();

    std::vector<VariantDPLHandle> extractMergingPipeline();

    BSONObj getSortPattern() const;

    static void assertVTableConstraints(const VTable_t& vtable) {
        tassert(ErrorCodes::InvalidExtensionVTable,
                "DistributedPlanLogic 'extract_shards_pipeline' is null",
                vtable.extract_shards_pipeline != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "DistributedPlanLogic 'extract_merging_pipeline' is null",
                vtable.extract_merging_pipeline != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "DistributedPlanLogic 'get_sort_pattern' is null",
                vtable.get_sort_pattern != nullptr);
    }
};

using DistributedPlanLogicHandle = OwnedHandle<::MongoExtensionDistributedPlanLogic>;
}  // namespace mongo::extension

