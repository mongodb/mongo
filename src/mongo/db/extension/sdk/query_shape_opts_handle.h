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
                       StringData fieldName,
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
