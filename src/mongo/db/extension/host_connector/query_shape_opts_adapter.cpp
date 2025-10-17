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
#include "mongo/db/extension/host_connector/query_shape_opts_adapter.h"

#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/extension_status.h"

namespace mongo::extension::host {

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
        *output = new VecByteBuf(reinterpret_cast<uint8_t*>(transformedIdentifier.data()),
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
        *output = new VecByteBuf(reinterpret_cast<uint8_t*>(transformedFieldPath.data()),
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
        *output = new VecByteBuf(reinterpret_cast<const uint8_t*>(transformedLiteral.rawdata()),
                                 transformedLiteral.size());
    });
}

}  // namespace mongo::extension::host
