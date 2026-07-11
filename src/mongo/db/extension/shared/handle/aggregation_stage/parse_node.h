// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/ast_node.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/modules.h"

#include <string_view>
#include <vector>

namespace mongo::extension {

class AggStageParseNodeAPI;

template <>
struct c_api_to_cpp_api<::MongoExtensionAggStageParseNode> {
    using CppApi_t = AggStageParseNodeAPI;
};

using AggStageParseNodeHandle = OwnedHandle<::MongoExtensionAggStageParseNode>;

using LogicalAggStageHandle = OwnedHandle<::MongoExtensionLogicalAggStage>;
/**
 * Represents the possible types of nodes created during expansion, as owned handles.
 *
 * Expansion can result in four types of nodes:
 * 1. Host-defined parse node
 * 2. Extension-defined parse node
 * 3. Host-defined AST node
 * 4. Extension-defined AST node
 *
 * This variant allows extension developers to return both host- and extension-defined nodes in
 * AggStageParseNode::expand() without knowing the underlying implementation of host-defined
 * nodes.
 *
 * The host is responsible for differentiating between host- and extension-defined nodes later
 * on.
 */
using VariantNodeHandle = std::variant<AggStageParseNodeHandle, AggStageAstNodeHandle>;

/**
 * Wrapper function that converts an ExpandedArray to a vector of RAII handles.
 * Declared here; defined in parse_node.cpp where ArrayElemAsRaii specializations are visible.
 */
std::vector<VariantNodeHandle> expandedArrayToRaiiVector(::MongoExtensionExpandedArray& arr);

/**
 * Represents the possible types of elements that can be in a MongoExtensionDPLArray, as owned
 * handles.
 *
 * A MongoExtensionDPLArray can contain either parse nodes or logical stages. This variant allows
 * extension developers to return both types in distributed plan logic.
 */
using VariantDPLHandle = std::variant<AggStageParseNodeHandle, LogicalAggStageHandle>;

/**
 * Wrapper function that converts a DPL array to a vector of RAII handles.
 * This ensures template instantiation happens in parse_node.cpp where
 * the specializations are visible.
 */
std::vector<VariantDPLHandle> dplArrayToRaiiVector(::MongoExtensionDPLArray& arr);

/**
 * AggStageParseNodeAPI is a wrapper around a MongoExtensionAggStageParseNode vtable API.
 */
class AggStageParseNodeAPI : public VTableAPI<::MongoExtensionAggStageParseNode> {
public:
    AggStageParseNodeAPI(::MongoExtensionAggStageParseNode* parseNode)
        : VTableAPI<::MongoExtensionAggStageParseNode>(parseNode) {}


    /**
     * Returns a std::string_view containing the name of this aggregation stage.
     */
    std::string_view getName() const {
        auto stringView = byteViewAsStringView(_vtable().get_name(get()));
        return std::string_view{stringView.data(), stringView.size()};
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts& opts) const;

    /**
     * Expands this parse node into its child nodes and returns them as a vector of host-side
     * handles.
     *
     * On success, the returned vector contains one or more handles (ParseNodeHandle or
     * AstNodeHandle) and ownership of the underlying nodes is transferred to the caller.
     *
     * On failure, the call triggers an assertion and no ownership is transferred.
     *
     * The caller is responsible for managing the lifetime of the returned handles.
     */
    std::vector<VariantNodeHandle> expand() const;

    /**
     * Clones this parse node into an identical parse node.
     *
     * On success, the ownership of the returned parse node is transferred to the caller.
     *
     * On failure, the call triggers an assertion and no ownership is transferred.
     *
     * The caller is responsible for managing the lifetime of the returned handle.
     */
    AggStageParseNodeHandle clone() const;

    BSONObj toBsonForLog() const;

    static void assertVTableConstraints(const VTable_t& vtable) {
        tassert(ErrorCodes::InvalidExtensionVTable,
                "AggStageParseNode 'get_name' is null",
                vtable.get_name != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "AggStageParseNode 'get_query_shape' is null",
                vtable.get_query_shape != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "AggStageParseNode 'expand' is null",
                vtable.expand != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "AggStageParseNode 'clone' is null",
                vtable.clone != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "AggStageParseNode 'to_bson_for_log' is null",
                vtable.to_bson_for_log != nullptr);
    }
};

}  // namespace mongo::extension
