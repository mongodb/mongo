// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
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

::MongoExtensionStatus* ExtensionAggStageParseNodeAdapter::_extExpand(
    const ::MongoExtensionAggStageParseNode* parseNode,
    ::MongoExtensionExpandedArrayContainer** expanded) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        auto expandedNodes =
            static_cast<const ExtensionAggStageParseNodeAdapter*>(parseNode)->getImpl().expand();
        sdk_tassert(11591602,
                    "AggStageParseNode expand() must return at least one element",
                    !expandedNodes.empty());
        *expanded = new ExtensionExpandedArrayContainerAdapter(
            ExpandedArrayContainer(std::move(expandedNodes)));
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
