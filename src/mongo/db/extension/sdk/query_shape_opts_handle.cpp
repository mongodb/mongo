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

#include "mongo/db/extension/sdk/query_shape_opts_handle.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"

namespace mongo::extension::sdk {

template <typename T>
T QueryShapeOptsHandle::serializeUsingOptsHelper(
    const MongoExtensionByteView* byteView,
    const std::function<MongoExtensionStatus*(const MongoExtensionHostQueryShapeOpts*,
                                              const MongoExtensionByteView*,
                                              ::MongoExtensionByteBuf**)>& apiFunc,
    const std::function<T(MongoExtensionByteView)>& transformViewToReturn) const {
    assertValid();

    ::MongoExtensionByteBuf* buf;
    auto* ptr = get();

    invokeCAndConvertStatusToException([&]() { return apiFunc(ptr, byteView, &buf); });

    tripwireAssert(
        11188202, "buffer returned from serialize function must not be null", buf != nullptr);

    // Take ownership of the returned buffer so that it gets cleaned up, then copy the memory
    // into a string to be returned.
    ExtensionByteBufHandle ownedBuf{buf};
    return transformViewToReturn(ownedBuf.getByteView());
}


std::string QueryShapeOptsHandle::serializeIdentifier(const std::string& identifier) const {
    auto byteView = stringViewAsByteView(identifier);

    auto transformViewToReturn = [](MongoExtensionByteView bv) {
        return std::string(byteViewAsStringView(bv));
    };

    return serializeUsingOptsHelper<std::string>(
        &byteView, vtable().serialize_identifier, transformViewToReturn);
}

std::string QueryShapeOptsHandle::serializeFieldPath(const std::string& fieldPath) const {
    auto byteView = stringViewAsByteView(fieldPath);

    auto transformViewToReturn = [](MongoExtensionByteView bv) {
        return std::string(byteViewAsStringView(bv));
    };

    return serializeUsingOptsHelper<std::string>(
        &byteView, vtable().serialize_field_path, transformViewToReturn);
}

void QueryShapeOptsHandle::appendLiteral(BSONObjBuilder& builder,
                                         StringData fieldName,
                                         const BSONElement& bsonElement) const {
    uint64_t bufSize = bsonElement.size();
    MongoExtensionByteView byteView{reinterpret_cast<const uint8_t*>(bsonElement.rawdata()),
                                    bufSize};

    // We return a bool from this function to conform to the `serializeUsingOptsHelper` template
    // structure. It would be ideal to return something like void() or void_t() but that does not
    // exist.
    auto transformViewToReturn = [&](MongoExtensionByteView bv) {
        BSONElement element(reinterpret_cast<const char*>(bv.data));
        builder.appendAs(element, fieldName);
        return true;
    };

    serializeUsingOptsHelper<bool>(&byteView, vtable().serialize_literal, transformViewToReturn);
}

}  // namespace mongo::extension::sdk
