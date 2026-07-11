// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/update/update_internal_node.h"
#include "mongo/util/modules.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo {

/**
 * An internal node in the prefix tree of update modifier expressions, representing updates to an
 * array using the $[<identifier>] syntax and array filters. See comment in class definition of
 * UpdateNode for more details.
 */
class UpdateArrayNode : public UpdateInternalNode {
public:
    /**
     * Creates a new UpdateArrayNode by merging two input UpdateArrayNode objects and their
     * children. Each child that lives on one side of the merge but not the other (according to the
     * array filter identifier) is cloned to the newly created UpdateArrayNode. Children that exist
     * on both sides of the merge get merged recursively before being added to the resulting
     * UpdateArrayNode. This merge operation is a deep copy: the new UpdateArrayNode is a brand new
     * tree that does not contain any references to the objects in the original input trees.
     * 'leftNode' and 'rightNode' are required to have the same array filters.
     */
    static std::unique_ptr<UpdateNode> createUpdateNodeByMerging(const UpdateArrayNode& leftNode,
                                                                 const UpdateArrayNode& rightNode,
                                                                 FieldRef* pathTaken);

    UpdateArrayNode(
        const std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>>& arrayFilters)
        : UpdateInternalNode(Type::Array), _arrayFilters(arrayFilters) {}

    std::unique_ptr<UpdateNode> clone() const final {
        return std::make_unique<UpdateArrayNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final {
        for (auto&& child : _children) {
            child.second->setCollator(collator);
        }
    }

    ApplyResult apply(ApplyParams applyParams,
                      UpdateNodeApplyParams updateNodeApplyParams) const final;

    UpdateNode* getChild(const std::string& field) const final;

    void setChild(std::string field, std::unique_ptr<UpdateNode> child) final;

    void produceSerializationMap(
        FieldRef* currentPath,
        std::map<std::string, std::vector<std::pair<std::string, BSONObj>>>*
            operatorOrientedUpdates,
        const query_shape::SerializationOptions& opts) const final {
        for (const auto& [pathSuffix, child] : _children) {
            FieldRef::FieldRefTempAppend tempAppend(*currentPath,
                                                    toArrayFilterIdentifier(pathSuffix));
            child->produceSerializationMap(currentPath, operatorOrientedUpdates, opts);
        }
    }

    void acceptVisitor(UpdateNodeVisitor* visitor) final {
        visitor->visit(this);
    }

private:
    const std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>>& _arrayFilters;
    std::map<std::string, clonable_ptr<UpdateNode>, pathsupport::cmpPathsAndArrayIndexes> _children;

    // When calling apply() causes us to merge elements of '_children', we store the result of the
    // merge in case we need it for another array element or document.
    mutable stdx::unordered_map<UpdateNode*,
                                stdx::unordered_map<UpdateNode*, clonable_ptr<UpdateNode>>>
        _mergedChildrenCache;
};

}  // namespace mongo
