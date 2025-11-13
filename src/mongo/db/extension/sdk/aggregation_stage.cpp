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
#include "mongo/db/extension/sdk/aggregation_stage.h"

#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/sdk/raii_vector_to_abi_array.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

namespace mongo::extension {

template <>
struct RaiiVectorElemType<::MongoExtensionExpandedArrayElement> {
    using type = VariantNodeHandle;
};

template <>
struct AbiArrayElemType<VariantNodeHandle> {
    using type = ::MongoExtensionExpandedArrayElement;
};

template <>
struct AbiArrayElemType<VariantDPLHandle> {
    using type = ::MongoExtensionDPLArrayElement;
};

namespace sdk {

MONGO_FAIL_POINT_DEFINE(failVariantNodeConversion);
MONGO_FAIL_POINT_DEFINE(failVariantDPLConversion);

namespace {

/**
 * Converts an SDK VariantNode into a tagged union of ABI objects and writes the raw pointers
 * into the host-allocated ExpandedArray element.
 */
struct ConsumeVariantNodeToAbi {
    ::MongoExtensionExpandedArrayElement& dst;

    void operator()(AggStageParseNodeHandle&& parseNode) const {
        dst.type = kParseNode;
        dst.parseOrAst.parse = parseNode.release();
    }

    void operator()(AggStageAstNodeHandle&& astNode) const {
        dst.type = kAstNode;
        dst.parseOrAst.ast = astNode.release();
    }
};

/**
 * Converts an SDK VariantDPLHandle into a tagged union of ABI objects and writes the raw pointers
 * into the host-allocated DPLArray element.
 */
struct ConsumeVariantDPLToAbi {
    ::MongoExtensionDPLArrayElement& dst;

    void operator()(AggStageParseNodeHandle&& parseNode) const {
        dst.type = kParse;
        dst.element.parseNode = parseNode.release();
    }

    void operator()(LogicalAggStageHandle&& logicalStage) const {
        dst.type = kLogical;
        dst.element.logicalStage = logicalStage.release();
    }
};
}  // namespace

template <>
struct RaiiAsArrayElem<VariantNodeHandle> {
    using VectorElem_t = VariantNodeHandle;
    using ArrayElem_t = AbiArrayElemType<VectorElem_t>::type;

    static void consume(ArrayElem_t& arrayElt, VectorElem_t&& vectorElt) {
        if (MONGO_unlikely(failVariantNodeConversion.shouldFail())) {
            sdk_uasserted(11197200,
                          "Injected failure in VariantNode conversion to ExpandedArrayElement");
        }
        std::visit(ConsumeVariantNodeToAbi{arrayElt}, std::move(vectorElt));
    }
};

::MongoExtensionStatus* ExtensionAggStageParseNode::_extExpand(
    const ::MongoExtensionAggStageParseNode* parseNode,
    ::MongoExtensionExpandedArray* expanded) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        const auto& impl = static_cast<const ExtensionAggStageParseNode*>(parseNode)->getImpl();
        const auto expandedSize = impl.getExpandedSize();
        sdk_tassert(11113801,
                    (str::stream()
                     << "MongoExtensionExpandedArray.size must equal required size: "
                     << "got " << expanded->size << ", but required " << expandedSize),
                    expanded->size == expandedSize);

        auto expandedNodes = impl.expand();
        raiiVectorToAbiArray(std::move(expandedNodes), *expanded);
    });
}

template <>
struct RaiiAsArrayElem<VariantDPLHandle> {
    using VectorElem_t = VariantDPLHandle;
    using ArrayElem_t = AbiArrayElemType<VectorElem_t>::type;

    static void consume(ArrayElem_t& arrayElt, VectorElem_t&& vectorElt) {
        if (MONGO_unlikely(failVariantDPLConversion.shouldFail())) {
            sdk_uasserted(11365501, "Injected failure in VariantDPL conversion to DPLArrayElement");
        }
        std::visit(ConsumeVariantDPLToAbi{arrayElt}, std::move(vectorElt));
    }
};

template void raiiVectorToAbiArray<VariantNodeHandle>(std::vector<VariantNodeHandle> inputVector,
                                                      ::MongoExtensionExpandedArray& outputArray);
template void raiiVectorToAbiArray<VariantDPLHandle>(std::vector<VariantDPLHandle> inputVector,
                                                     ::MongoExtensionDPLArray& outputArray);

}  // namespace sdk
}  // namespace mongo::extension
