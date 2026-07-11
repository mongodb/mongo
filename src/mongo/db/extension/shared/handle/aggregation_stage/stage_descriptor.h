// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <absl/base/nullability.h>

namespace mongo::extension {

class AggStageDescriptorAPI;

template <>
struct c_api_to_cpp_api<::MongoExtensionAggStageDescriptor> {
    using CppApi_t = AggStageDescriptorAPI;
};

/**
 * AggStageDescriptorHandle is a wrapper around a MongoExtensionAggStageDescriptor vtable API.
 */
class AggStageDescriptorAPI : public VTableAPI<::MongoExtensionAggStageDescriptor> {
public:
    AggStageDescriptorAPI(::MongoExtensionAggStageDescriptor* descriptor)
        : VTableAPI<::MongoExtensionAggStageDescriptor>(descriptor) {}

    /**
     * Returns a std::string_view containing the name of this aggregation stage.
     */
    std::string_view getName() const {
        auto stringView = byteViewAsStringView(_vtable().get_name(get()));
        return std::string_view{stringView.data(), stringView.size()};
    }

    /**
     * Parse the user provided stage definition for this stage descriptor.
     *
     * stageBson contains a BSON document with a single (stageName, stageDefinition) element
     * tuple.
     *
     * On success, the parse node is returned and belongs to the caller.
     * On failure, the error triggers an assertion.
     *
     */
    AggStageParseNodeHandle parse(BSONObj stageBson) const;

    /**
     * Returns the type of client permitted to specify this stage in a command request. Queried at
     * registration time to decide whether the stage is internal-only.
     */
    ::MongoExtensionClientType getClientType() const {
        return _vtable().get_client_type(get());
    }

    static void assertVTableConstraints(const VTable_t& vtable) {
        tassert(ErrorCodes::InvalidExtensionVTable,
                "ExtensionAggStageDescriptorAdapter 'get_name' is null",
                vtable.get_name != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "ExtensionAggStageDescriptorAdapter 'parse' is null",
                vtable.parse != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "ExtensionAggStageDescriptorAdapter 'get_client_type' is null",
                vtable.get_client_type != nullptr);
    }
};

using AggStageDescriptorHandle = UnownedHandle<const ::MongoExtensionAggStageDescriptor>;

}  // namespace mongo::extension
