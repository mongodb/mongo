// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <absl/base/nullability.h>

namespace mongo::extension {

class ExtensionByteBufAPI;

template <>
struct c_api_to_cpp_api<::MongoExtensionByteBuf> {
    using CppApi_t = ExtensionByteBufAPI;
};

/**
 * ExtensionByteBufVTable is a wrapper around a MongoExtensionByteBuf.
 */
class ExtensionByteBufAPI : public VTableAPI<::MongoExtensionByteBuf> {
public:
    ExtensionByteBufAPI(::MongoExtensionByteBuf* byteBufPtr)
        : VTableAPI<::MongoExtensionByteBuf>(byteBufPtr) {}

    /**
     * Get a read-only byte view of the contents of ByteBuf.
     */
    MongoExtensionByteView getByteView() const {
        return _vtable().get_view(get());
    }

    /**
     * Get a read-only string view of the contents of ByteBuf.
     */
    std::string_view getStringView() const {
        return byteViewAsStringView(_vtable().get_view(get()));
    }

    static void assertVTableConstraints(const VTable_t& vtable) {
        tassert(ErrorCodes::InvalidExtensionVTable,
                "ByteBuf 'get_view' is null",
                vtable.get_view != nullptr);
    };
};

using ExtensionByteBufHandle = OwnedHandle<::MongoExtensionByteBuf>;
}  // namespace mongo::extension
