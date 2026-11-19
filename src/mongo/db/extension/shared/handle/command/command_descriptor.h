// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::extension {

class CommandDescriptorAPI;

template <>
struct c_api_to_cpp_api<::MongoExtensionCommandDescriptor> {
    using CppApi_t = CommandDescriptorAPI;
};

// Commands were introduced in API v1.1.
template <>
struct c_api_to_min_supported_capability<::MongoExtensionCommandDescriptor>
    : std::integral_constant<uint32_t, static_cast<uint32_t>(CapabilityVersion(1, 1))> {};

/**
 * CommandDescriptorAPI is a wrapper around a MongoExtensionCommandDescriptor vtable API.
 */
class CommandDescriptorAPI : public VTableAPI<::MongoExtensionCommandDescriptor> {
public:
    CommandDescriptorAPI(::MongoExtensionCommandDescriptor* descriptor,
                         const CapabilityVersion& capability)
        : VTableAPI<::MongoExtensionCommandDescriptor>(descriptor, capability) {}

    /**
     * Returns a std::string_view containing the name of this command. The view is valid for the
     * lifetime of the descriptor.
     */
    std::string_view getName() const {
        return byteViewAsStringView(_vtable().get_name(get()));
    }

    /**
     * Returns an owned BSON document containing the command's static properties.
     */
    BSONObj getProperties() const;

    /**
     * Runs the command with the given request, context, and execution context, appending the
     * extension's response to 'result'.
     *
     * On failure, the error triggers an assertion and nothing is appended to 'result'.
     */
    void run(const BSONObj& requestBson,
             const ::MongoExtensionCommandContext& ctx,
             const ::MongoExtensionCommandExecutionContext& execCtx,
             BSONObjBuilder& result) const;

    static void assertVTableConstraints(const VTable_t& vtable, const CapabilityVersion&) {
        tassert(ErrorCodes::InvalidExtensionVTable,
                "ExtensionCommandDescriptor 'get_name' is null",
                vtable.get_name != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "ExtensionCommandDescriptor 'get_properties' is null",
                vtable.get_properties != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "ExtensionCommandDescriptor 'run' is null",
                vtable.run != nullptr);
    }
};

using CommandDescriptorHandle = UnownedHandle<const ::MongoExtensionCommandDescriptor>;

}  // namespace mongo::extension
