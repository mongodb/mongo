// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/sdk/query_shape_opts_handle.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"

#include <string_view>

namespace mongo::extension::sdk {

template <typename T>
T QueryShapeOptsAPI::serializeUsingOptsHelper(
    MongoExtensionByteView byteView,
    const std::function<MongoExtensionStatus*(const MongoExtensionHostQueryShapeOpts*,
                                              MongoExtensionByteView,
                                              ::MongoExtensionByteBuf**)>& apiFunc,
    const std::function<T(MongoExtensionByteView)>& transformViewToReturn) const {
    assertValid();

    ::MongoExtensionByteBuf* buf{nullptr};

    invokeCAndConvertStatusToException([&]() { return apiFunc(get(), byteView, &buf); });

    sdk_tassert(
        11188202, "buffer returned from serialize function must not be null", buf != nullptr);

    // Take ownership of the returned buffer so that it gets cleaned up, then copy the memory
    // into a string to be returned.
    ExtensionByteBufHandle ownedBuf{buf};
    return transformViewToReturn(ownedBuf->getByteView());
}


std::string QueryShapeOptsAPI::serializeIdentifier(const std::string& identifier) const {
    auto byteView = stringViewAsByteView(identifier);

    auto transformViewToReturn = [](MongoExtensionByteView bv) {
        return std::string(byteViewAsStringView(bv));
    };

    return serializeUsingOptsHelper<std::string>(
        byteView, _vtable().serialize_identifier, transformViewToReturn);
}

std::string QueryShapeOptsAPI::serializeFieldPath(const std::string& fieldPath) const {
    auto byteView = stringViewAsByteView(fieldPath);

    auto transformViewToReturn = [](MongoExtensionByteView bv) {
        return std::string(byteViewAsStringView(bv));
    };

    return serializeUsingOptsHelper<std::string>(
        byteView, _vtable().serialize_field_path, transformViewToReturn);
}

void QueryShapeOptsAPI::appendLiteral(BSONObjBuilder& builder,
                                      std::string_view fieldName,
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

    serializeUsingOptsHelper<bool>(byteView, _vtable().serialize_literal, transformViewToReturn);
}

}  // namespace mongo::extension::sdk
