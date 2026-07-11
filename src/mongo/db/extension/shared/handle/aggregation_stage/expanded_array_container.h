// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

namespace mongo::extension {

class ExpandedArrayContainerAPI;

template <>
struct c_api_to_cpp_api<::MongoExtensionExpandedArrayContainer> {
    using CppApi_t = ExpandedArrayContainerAPI;
};

/**
 * ExpandedArrayContainerAPI is a wrapper around a MongoExtensionExpandedArrayContainer.
 */
class ExpandedArrayContainerAPI : public VTableAPI<::MongoExtensionExpandedArrayContainer> {
public:
    ExpandedArrayContainerAPI(::MongoExtensionExpandedArrayContainer* container)
        : VTableAPI<::MongoExtensionExpandedArrayContainer>(container) {}

    size_t size() const {
        return _vtable().size(get());
    }

    /**
     * Transfers ownership of the elements in the container into a vector of RAII handles.
     */
    std::vector<VariantNodeHandle> transfer() {
        const auto arraySize = size();
        std::vector<::MongoExtensionExpandedArrayElement> sdkAbiArray{arraySize};

        ::MongoExtensionExpandedArray targetArray{arraySize, sdkAbiArray.data()};
        _transferInternal(targetArray);

        return expandedArrayToRaiiVector(targetArray);
    }

    static void assertVTableConstraints(const VTable_t& vtable) {
        tassert(ErrorCodes::InvalidExtensionVTable,
                "ExpandedArrayContainer 'size' is null",
                vtable.size != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "ExpandedArrayContainer 'transfer' is null",
                vtable.transfer != nullptr);
    }

protected:
    /**
     * Internal helper for transferring to a pre-allocated array.
     * Callers of this function must have already verified isValid().
     */
    void _transferInternal(::MongoExtensionExpandedArray& arr) {
        invokeCAndConvertStatusToException([&]() { return _vtable().transfer(get(), &arr); });
    }
};

using ExpandedArrayContainerHandle = OwnedHandle<::MongoExtensionExpandedArrayContainer>;

}  // namespace mongo::extension
