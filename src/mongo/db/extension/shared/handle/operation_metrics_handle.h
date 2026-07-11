// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"
#include "mongo/util/modules.h"

namespace mongo::extension {

class OperationMetricsAPI;

template <>
struct c_api_to_cpp_api<::MongoExtensionOperationMetrics> {
    using CppApi_t = OperationMetricsAPI;
};

using OwnedOperationMetricsHandle = OwnedHandle<::MongoExtensionOperationMetrics>;
using UnownedOperationMetricsHandle = UnownedHandle<::MongoExtensionOperationMetrics>;

class OperationMetricsAPI : public VTableAPI<::MongoExtensionOperationMetrics> {
public:
    OperationMetricsAPI(::MongoExtensionOperationMetrics* ctx)
        : VTableAPI<::MongoExtensionOperationMetrics>(ctx) {}

    // Call the underlying object's serialize function and return the resulting BSON object.
    BSONObj serialize() const {
        ::MongoExtensionByteBuf* buf{nullptr};

        invokeCAndConvertStatusToException([&]() { return _vtable().serialize(get(), &buf); });

        tassert(ErrorCodes::ExtensionSerializationError,
                "buffer returned from serialize function must not be null",
                buf != nullptr);

        // Take ownership of the returned buffer so that it gets cleaned up, then copy the memory
        // into a BSON object to be returned.
        ExtensionByteBufHandle ownedBuf{buf};
        return bsonObjFromByteView(ownedBuf->getByteView()).getOwned();
    }

    void update(const MongoExtensionByteView& byteView) {
        invokeCAndConvertStatusToException([&]() { return _vtable().update(get(), byteView); });
    }

    static void assertVTableConstraints(const VTable_t& vtable) {
        tassert(ErrorCodes::InvalidExtensionVTable,
                "HostOperationMetrics' 'serialize' is null",
                vtable.serialize != nullptr);
    };
};
}  // namespace mongo::extension
