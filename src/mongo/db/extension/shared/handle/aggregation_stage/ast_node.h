// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/public/extension_agg_stage_static_properties_gen.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/logical.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <absl/base/nullability.h>

namespace mongo::extension {

class AggStageAstNodeAPI;

using AggStageAstNodeHandle = OwnedHandle<::MongoExtensionAggStageAstNode>;

template <>
struct c_api_to_cpp_api<::MongoExtensionAggStageAstNode> {
    using CppApi_t = AggStageAstNodeAPI;
};

/**
 * AggStageAstNodeAPI is a wrapper around a MongoExtensionAggStageAstNode vtable API.
 */
class AggStageAstNodeAPI : public VTableAPI<::MongoExtensionAggStageAstNode> {
public:
    AggStageAstNodeAPI(::MongoExtensionAggStageAstNode* ptr)
        : VTableAPI<::MongoExtensionAggStageAstNode>(ptr) {}

    /**
     * Returns a std::string_view containing the name of this aggregation stage.
     */
    std::string_view getName() const {
        auto stringView = byteViewAsStringView(_vtable().get_name(get()));
        return std::string_view{stringView.data(), stringView.size()};
    }

    MongoExtensionStaticProperties getProperties() const;

    /**
     * Returns a logical stage with the stage's runtime implementation of the optimization
     * interface.
     *
     * On success, the logical stage is returned and belongs to the caller.
     * On failure, the error triggers an assertion.
     *
     */
    LogicalAggStageHandle promote(const ::MongoExtensionCatalogContext& catalogContext) const;

    /**
     * Clones this AST node into an identical AST node.
     *
     * On success, the ownership of the returned AST node is transferred to the caller.
     *
     * On failure, the call triggers an assertion and no ownership is transferred.
     *
     * The caller is responsible for managing the lifetime of the returned handle.
     */
    AggStageAstNodeHandle clone() const;

    /**
     * Retrieves the FirstStageViewApplication policy for this AST node
     */
    MongoExtensionFirstStageViewApplicationPolicy getFirstStageViewApplicationPolicy() const;

    /**
     * Binds view information to this AST node.
     */
    void bindResolvedNamespace(const ::MongoExtensionResolvedNamespace& resolvedNamespace);

    static void assertVTableConstraints(const VTable_t& vtable) {
        tassert(ErrorCodes::InvalidExtensionVTable,
                "AggStageAstNode 'get_name' is null",
                vtable.get_name != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "AggStageAstNode 'get_properties' is null",
                vtable.get_properties != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "AggStageAstNode 'promote' is null",
                vtable.promote != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "AggStageAstNode 'clone' is null",
                vtable.clone != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "AggStageAstNode 'get_first_stage_view_application_policy` is null",
                vtable.get_first_stage_view_application_policy != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "AggStageAstNode `bind_resolved_namespace` is null",
                vtable.bind_resolved_namespace != nullptr);
    }
};

}  // namespace mongo::extension
