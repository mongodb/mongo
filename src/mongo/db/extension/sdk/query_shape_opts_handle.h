// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::extension {
namespace sdk {
class QueryShapeOptsAPI;
}

template <>
struct c_api_to_cpp_api<::MongoExtensionHostQueryShapeOpts> {
    using CppApi_t = sdk::QueryShapeOptsAPI;
};

namespace sdk {

/**
 * Wrapper for ::MongoExtensionHostQueryShapeOpts, providing safe access to its public API through
 * the underlying vtable.
 *
 * Typically ownership of MongoExtensionHostQueryShapeOpts pointer is never transferred to the
 * extension, so this API is expected to only be used with an UnownedHandle.
 */
class QueryShapeOptsAPI : public VTableAPI<::MongoExtensionHostQueryShapeOpts> {
public:
    QueryShapeOptsAPI(::MongoExtensionHostQueryShapeOpts* ctx)
        : VTableAPI<::MongoExtensionHostQueryShapeOpts>(ctx) {}

    std::string serializeIdentifier(const std::string& identifier) const;

    std::string serializeFieldPath(const std::string& fieldPath) const;

    void appendLiteral(BSONObjBuilder& builder,
                       std::string_view fieldName,
                       const BSONElement& bsonElement) const;

    static void assertVTableConstraints(const VTable_t& vtable) {
        sdk_tassert(11136800,
                    "HostQueryShapeOpts' 'serializeIdentifier' is null",
                    vtable.serialize_identifier != nullptr);
        sdk_tassert(11173400,
                    "HostQueryShapeOpts' 'serializeFieldPath' is null",
                    vtable.serialize_field_path != nullptr);
        sdk_tassert(11173500,
                    "HostQueryShapeOpts' 'serializeLiteral' is null",
                    vtable.serialize_literal != nullptr);
    };

private:
    /**
     * A templated helper function for vtable functions that take a byte view and populate a byte
     * buf output. The `transformViewToReturn` callback receives read-only byte view extracted
     * from the populated buffer and transforms it into an object of type T.
     */
    template <typename T>
    T serializeUsingOptsHelper(
        MongoExtensionByteView byteViewToSerialize,
        const std::function<MongoExtensionStatus*(const MongoExtensionHostQueryShapeOpts*,
                                                  MongoExtensionByteView,
                                                  ::MongoExtensionByteBuf**)>& apiFunc,
        const std::function<T(MongoExtensionByteView)>& transformViewToReturn) const;
};

using QueryShapeOptsHandle = UnownedHandle<const ::MongoExtensionHostQueryShapeOpts>;

}  // namespace sdk
}  // namespace mongo::extension
