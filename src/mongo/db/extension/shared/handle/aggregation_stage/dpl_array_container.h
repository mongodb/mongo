// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

namespace mongo::extension {

class DPLArrayContainerAPI;

template <>
struct c_api_to_cpp_api<::MongoExtensionDPLArrayContainer> {
    using CppApi_t = DPLArrayContainerAPI;
};

/**
 * DPLArrayContainerHandle is a wrapper around a MongoExtensionDPLArrayContainer.
 */
class DPLArrayContainerAPI : public VTableAPI<::MongoExtensionDPLArrayContainer> {
public:
    DPLArrayContainerAPI(::MongoExtensionDPLArrayContainer* container)
        : VTableAPI<::MongoExtensionDPLArrayContainer>(container) {}

    size_t size() const {
        return _vtable().size(get());
    }

    /**
     * Transfers ownership of the elements in the container into a vector of RAII handles.
     */
    std::vector<VariantDPLHandle> transfer() {
        const auto arraySize = size();
        std::vector<::MongoExtensionDPLArrayElement> sdkAbiArray{arraySize};

        ::MongoExtensionDPLArray targetArray{arraySize, sdkAbiArray.data()};
        _transferInternal(targetArray);

        return dplArrayToRaiiVector(targetArray);
    }

    static void assertVTableConstraints(const VTable_t& vtable) {
        tassert(ErrorCodes::InvalidExtensionVTable,
                "DPLArrayContainer 'size' is null",
                vtable.size != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "DPLArrayContainer 'transfer' is null",
                vtable.transfer != nullptr);
    }

protected:
    /**
     * Internal helper for transferring to a pre-allocated array.
     * Callers of this function must have already verified isValid().
     */
    void _transferInternal(::MongoExtensionDPLArray& arr) {
        invokeCAndConvertStatusToException([&]() { return _vtable().transfer(get(), &arr); });
    }
};

using DPLArrayContainerHandle = OwnedHandle<::MongoExtensionDPLArrayContainer>;

}  // namespace mongo::extension

