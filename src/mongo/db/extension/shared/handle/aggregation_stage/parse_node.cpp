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
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"

#include "mongo/db/extension/shared/array/abi_array_to_raii_vector.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

namespace mongo::extension {

MONGO_FAIL_POINT_DEFINE(failExtensionExpand);
MONGO_FAIL_POINT_DEFINE(failExtensionDPL);

BSONObj AggStageParseNodeAPI::getQueryShape(const ::MongoExtensionHostQueryShapeOpts& opts) const {
    ::MongoExtensionByteBuf* buf{nullptr};

    invokeCAndConvertStatusToException(
        [&]() { return vtable().get_query_shape(get(), &opts, &buf); });

    tassert(11188203, "buffer returned from get_query_shape must not be null", buf != nullptr);

    // Take ownership of the returned buffer so that it gets cleaned up, then retrieve an owned
    // BSONObj to return to the host.
    ExtensionByteBufHandle ownedBuf{buf};
    return bsonObjFromByteView(ownedBuf->getByteView()).getOwned();
}

template <>
struct RaiiVectorElemType<::MongoExtensionExpandedArrayElement> {
    using type = VariantNodeHandle;
};

template <>
struct ArrayElemAsRaii<::MongoExtensionExpandedArrayElement> {
    using ArrayElem_t = ::MongoExtensionExpandedArrayElement;
    using RaiiElem_t = RaiiVectorElemType<ArrayElem_t>::type;
    static RaiiElem_t consume(ArrayElem_t& elt) {
        VariantNodeHandle handle = AggStageParseNodeHandle{nullptr};
        if (MONGO_unlikely(failExtensionExpand.shouldFail())) {
            uasserted(11113805, "Injected failure in expand() during handle transfer");
        }
        switch (elt.type) {
            case kParseNode: {
                handle = VariantNodeHandle(AggStageParseNodeHandle{elt.parseOrAst.parse});
                elt.parseOrAst.parse = nullptr;
                break;
            }
            case kAstNode: {
                handle = VariantNodeHandle(AggStageAstNodeHandle{elt.parseOrAst.ast});
                elt.parseOrAst.ast = nullptr;
                break;
            }
            default:
                tasserted(11113804, "ExpandedArray element has invalid type tag");
                break;
        }
        return handle;
    }
};

std::vector<VariantNodeHandle> AggStageParseNodeAPI::expand() const {
    // Host allocates buffer with the expected size.
    const auto expandedSize = getExpandedSize();
    tassert(11113803, "AggStageParseNode getExpandedSize() must be >= 1", expandedSize >= 1);
    std::vector<::MongoExtensionExpandedArrayElement> buf{expandedSize};
    ::MongoExtensionExpandedArray expandedArray{expandedSize, buf.data()};
    invokeCAndConvertStatusToException([&] { return vtable().expand(get(), &expandedArray); });
    return abiArrayToRaiiVector(expandedArray);
}

template <>
struct RaiiVectorElemType<::MongoExtensionDPLArrayElement> {
    using type = VariantDPLHandle;
};

template <>
struct ArrayElemAsRaii<::MongoExtensionDPLArrayElement> {
    using ArrayElem_t = ::MongoExtensionDPLArrayElement;
    using RaiiElem_t = RaiiVectorElemType<ArrayElem_t>::type;
    static RaiiElem_t consume(ArrayElem_t& elt) {
        VariantDPLHandle handle = AggStageParseNodeHandle{nullptr};
        if (MONGO_unlikely(failExtensionDPL.shouldFail())) {
            uasserted(11365502, "Injected failure in DPL during handle transfer");
        }
        switch (elt.type) {
            case kParse: {
                handle = VariantDPLHandle(AggStageParseNodeHandle{elt.element.parseNode});
                elt.element.parseNode = nullptr;
                break;
            }
            case kLogical: {
                handle = VariantDPLHandle(LogicalAggStageHandle{elt.element.logicalStage});
                elt.element.logicalStage = nullptr;
                break;
            }
            default:
                tasserted(11365500, "DPLArray element has invalid type tag");
                break;
        }
        return handle;
    }
};

std::vector<VariantDPLHandle> dplArrayToRaiiVector(::MongoExtensionDPLArray& arr) {
    return abiArrayToRaiiVector(arr);
}

}  // namespace mongo::extension
