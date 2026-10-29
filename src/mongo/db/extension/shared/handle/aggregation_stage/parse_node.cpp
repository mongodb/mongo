// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"

#include "mongo/db/extension/shared/array/abi_array_to_raii_vector.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/expanded_array_container.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

namespace mongo::extension {

MONGO_FAIL_POINT_DEFINE(failExtensionExpand);
MONGO_FAIL_POINT_DEFINE(failExtensionDPL);

BSONObj AggStageParseNodeAPI::getQueryShape(const ::MongoExtensionHostQueryShapeOpts& opts) const {
    ::MongoExtensionByteBuf* buf{nullptr};

    invokeCAndConvertStatusToException(
        [&]() { return _vtable().get_query_shape(get(), &opts, &buf); });

    tassert(ErrorCodes::ExtensionSerializationError,
            "buffer returned from get_query_shape must not be null",
            buf != nullptr);

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
                tasserted(ErrorCodes::ExtensionError, "ExpandedArray element has invalid type tag");
                break;
        }
        return handle;
    }
};

std::vector<VariantNodeHandle> AggStageParseNodeAPI::expand() const {
    ::MongoExtensionExpandedArrayContainer* container = nullptr;
    invokeCAndConvertStatusToException([&]() { return _vtable().expand(get(), &container); });
    tassert(ErrorCodes::ExtensionError,
            "Container cannot be null after expand()",
            container != nullptr);
    ExpandedArrayContainerHandle handle(container);
    return handle->transfer();
}

std::vector<VariantNodeHandle> expandedArrayToRaiiVector(::MongoExtensionExpandedArray& arr) {
    return abiArrayToRaiiVector(arr);
}

AggStageParseNodeHandle AggStageParseNodeAPI::clone() const {
    assertValid();
    ::MongoExtensionAggStageParseNode* parseNodePtr{nullptr};

    // The API's contract mandates that parseNodePtr will only be allocated if status is OK.
    invokeCAndConvertStatusToException([&] { return _vtable().clone(get(), &parseNodePtr); });

    return AggStageParseNodeHandle(parseNodePtr);
}

BSONObj AggStageParseNodeAPI::toBsonForLog() const {
    ::MongoExtensionByteBuf* buf{nullptr};
    invokeCAndConvertStatusToException([&]() { return _vtable().to_bson_for_log(get(), &buf); });

    tassert(ErrorCodes::ExtensionSerializationError,
            "Extension implementation of `to_bson_for_log` encountered nullptr",
            buf != nullptr);

    ExtensionByteBufHandle ownedBuf{buf};
    return bsonObjFromByteView(ownedBuf->getByteView()).getOwned();
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
                tasserted(ErrorCodes::ExtensionError, "DPLArray element has invalid type tag");
                break;
        }
        return handle;
    }
};

std::vector<VariantDPLHandle> dplArrayToRaiiVector(::MongoExtensionDPLArray& arr) {
    return abiArrayToRaiiVector(arr);
}

}  // namespace mongo::extension
