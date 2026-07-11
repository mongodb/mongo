// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/extension/shared/handle/aggregation_stage/ast_node.h"

#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"

namespace mongo::extension {

MongoExtensionStaticProperties AggStageAstNodeAPI::getProperties() const {
    ::MongoExtensionByteBuf* buf{nullptr};
    invokeCAndConvertStatusToException([&]() { return _vtable().get_properties(get(), &buf); });

    tassert(
        11347802,
        "Extension implementation of `getProperties` encountered nullptr inside the output buffer.",
        buf != nullptr);

    ExtensionByteBufHandle ownedBuf{buf};
    const auto propertiesObj = bsonObjFromByteView(ownedBuf->getByteView());
    return MongoExtensionStaticProperties::parse(propertiesObj);
}

LogicalAggStageHandle AggStageAstNodeAPI::promote(
    const ::MongoExtensionCatalogContext& catalogContext) const {
    ::MongoExtensionLogicalAggStage* logicalStagePtr{nullptr};

    // The API's contract mandates that logicalStagePtr will only be allocated if status is OK.
    invokeCAndConvertStatusToException(
        [&]() { return _vtable().promote(get(), &catalogContext, &logicalStagePtr); });

    return LogicalAggStageHandle(logicalStagePtr);
}

AggStageAstNodeHandle AggStageAstNodeAPI::clone() const {
    assertValid();
    ::MongoExtensionAggStageAstNode* astNodePtr{nullptr};

    // The API's contract mandates that astNodePtr will only be allocated if status is OK.
    invokeCAndConvertStatusToException([&]() { return _vtable().clone(get(), &astNodePtr); });

    return AggStageAstNodeHandle(astNodePtr);
}

MongoExtensionFirstStageViewApplicationPolicy
AggStageAstNodeAPI::getFirstStageViewApplicationPolicy() const {
    MongoExtensionFirstStageViewApplicationPolicy policy =
        MongoExtensionFirstStageViewApplicationPolicy::kDefaultPrepend;
    invokeCAndConvertStatusToException(
        [&]() { return _vtable().get_first_stage_view_application_policy(get(), &policy); });
    return policy;
}

void AggStageAstNodeAPI::bindResolvedNamespace(
    const ::MongoExtensionResolvedNamespace& resolvedNamespace) {
    invokeCAndConvertStatusToException(
        [&]() { return _vtable().bind_resolved_namespace(get(), &resolvedNamespace); });
}
}  // namespace mongo::extension
