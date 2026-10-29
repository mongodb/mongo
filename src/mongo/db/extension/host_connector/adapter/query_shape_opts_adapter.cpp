// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/extension/host_connector/adapter/query_shape_opts_adapter.h"

#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/extension_status.h"

namespace mongo::extension::host_connector {

MongoExtensionStatus* QueryShapeOptsAdapter::_extSerializeIdentifier(
    const ::MongoExtensionHostQueryShapeOpts* ctx,
    ::MongoExtensionByteView identifier,
    ::MongoExtensionByteBuf** output) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        *output = nullptr;

        const auto& opts = static_cast<const QueryShapeOptsAdapter*>(ctx)->getOptsImpl();
        auto transformedIdentifier =
            opts->serializeIdentifier(std::string(byteViewAsStringView(identifier)));

        // Allocate a buffer on the heap. Ownership is transferred to the caller.
        *output = new ByteBuf(reinterpret_cast<uint8_t*>(transformedIdentifier.data()),
                              transformedIdentifier.length());
    });
}

MongoExtensionStatus* QueryShapeOptsAdapter::_extSerializeFieldPath(
    const ::MongoExtensionHostQueryShapeOpts* ctx,
    ::MongoExtensionByteView fieldPath,
    ::MongoExtensionByteBuf** output) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        *output = nullptr;

        const auto& opts = static_cast<const QueryShapeOptsAdapter*>(ctx)->getOptsImpl();
        auto transformedFieldPath =
            opts->serializeFieldPath(std::string(byteViewAsStringView(fieldPath)));

        // Allocate a buffer on the heap. Ownership is transferred to the caller.
        *output = new ByteBuf(reinterpret_cast<uint8_t*>(transformedFieldPath.data()),
                              transformedFieldPath.length());
    });
}

MongoExtensionStatus* QueryShapeOptsAdapter::_extSerializeLiteral(
    const ::MongoExtensionHostQueryShapeOpts* ctx,
    ::MongoExtensionByteView bsonElement,
    ::MongoExtensionByteBuf** output) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        *output = nullptr;

        const auto& opts = static_cast<const QueryShapeOptsAdapter*>(ctx)->getOptsImpl();

        // Parse a BSONElement out of the ptr passed from the extension. Note that the caller must
        // ensure that underlying BSONObj remains valid.
        BSONElement rawLiteral(reinterpret_cast<const char*>(bsonElement.data));

        // Serialize the literal and then copy it into a BSON object. The BSON serialization is
        // necessary because the Value returned can be any valid Value type, and the easiest way to
        // have the extension deduce the return type is to rely on our existing BSON infrastructure.
        auto val = opts->serializeLiteral(rawLiteral);

        // Serialize the Value into a BSON document.
        MutableDocument doc;
        doc.addField("", std::move(val));
        auto bson = doc.freeze().toBson();

        // Allocate a buffer on the heap for the output BSONElement with the serialized literal.
        // Ownership is transferred to the caller.
        BSONElement transformedLiteral = bson.firstElement();
        *output = new ByteBuf(reinterpret_cast<const uint8_t*>(transformedLiteral.rawdata()),
                              transformedLiteral.size());
    });
}

}  // namespace mongo::extension::host_connector
