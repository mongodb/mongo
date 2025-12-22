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
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/ast_node.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/modules.h"

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
 * AggStageParseNodeHandle is a wrapper around a MongoExtensionAggStageParseNode vtable API.
 */
class AggStageParseNodeAPI : public VTableAPI<::MongoExtensionAggStageParseNode> {
public:
    AggStageParseNodeAPI(::MongoExtensionAggStageParseNode* parseNode)
        : VTableAPI<::MongoExtensionAggStageParseNode>(parseNode) {}


    /**
     * Returns a StringData containing the name of this aggregation stage.
     */
    StringData getName() const {
        auto stringView = byteViewAsStringView(vtable().get_name(get()));
        return StringData{stringView.data(), stringView.size()};
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

    static void assertVTableConstraints(const VTable_t& vtable) {
        tassert(11217600, "AggStageParseNode 'get_name' is null", vtable.get_name != nullptr);
        tassert(10977600,
                "AggStageParseNode 'get_query_shape' is null",
                vtable.get_query_shape != nullptr);
        tassert(11113800,
                "AggStageParseNode 'get_expanded_size' is null",
                vtable.get_expanded_size != nullptr);
        tassert(10977601, "AggStageParseNode 'expand' is null", vtable.expand != nullptr);
    }

private:
    /**
     * Returns the number of elements the parse node will produce when expanded. This value is used
     * by the host to size the ExpandedArray.
     */
    size_t getExpandedSize() const {
        return vtable().get_expanded_size(get());
    }
};
}  // namespace mongo::extension
