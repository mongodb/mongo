// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/update/modifier_table.h"
#include "mongo/db/update/path_support.h"
#include "mongo/db/update/update_internal_node.h"
#include "mongo/stdx/unordered_map.h"
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
 * object. See comment in class definition of UpdateNode for more details.
 */
class UpdateObjectNode : public UpdateInternalNode {

public:
    /**
     * Parses 'modExpr' as an update modifier expression and merges with it with 'root'. Returns a
     * non-OK status if 'modExpr' is not a valid update modifier expression, if merging would
     * cause a conflict, or if there is an array filter identifier in 'modExpr' without a
     * corresponding filter in 'arrayFilters'. Returns true if the path of 'modExpr' contains a
     * positional $ element, e.g. 'a.$.b'. Any array filter identifiers are added to
     * 'foundIdentifiers'.
     */
    static StatusWith<bool> parseAndMerge(
        UpdateObjectNode* root,
        modifiertable::ModifierType type,
        BSONElement modExpr,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>>& arrayFilters,
        std::set<std::string>& foundIdentifiers);

    /**
     * Parses a field of the form $[<identifier>] into <identifier>. 'field' must be of the form
     * $[<identifier>]. Returns a non-ok status if 'field' is in the first position in the path or
     * the array filter identifier does not have a corresponding filter in 'arrayFilters'. Adds the
     * identifier to 'foundIdentifiers'.
     */
    static StatusWith<std::string> parseArrayFilterIdentifier(
        std::string_view field,
        size_t position,
        const FieldRef& fieldRef,
        const std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>>& arrayFilters,
        std::set<std::string>& foundIdentifiers);

    /**
     * Creates a new UpdateObjectNode by merging two input UpdateObjectNode objects and their
     * children. Each field that lives on one side of the merge but not the other (according to
     * field name) is cloned to the newly created UpdateObjectNode. Fields that exist on both sides
     * of the merge get merged recursively before being added to the resulting UpdateObjectNode.
     * This merge operation is a deep copy: the new UpdateObjectNode is a brand new tree that does
     * not contain any references to the objects in the original input trees.
     */
    static std::unique_ptr<UpdateNode> createUpdateNodeByMerging(const UpdateObjectNode& leftNode,
                                                                 const UpdateObjectNode& rightNode,
                                                                 FieldRef* pathTaken);

    UpdateObjectNode() : UpdateInternalNode(Type::Object) {}

    std::unique_ptr<UpdateNode> clone() const final {
        return std::make_unique<UpdateObjectNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final {
        for (auto&& child : _children) {
            child.second->setCollator(collator);
        }
        if (_positionalChild) {
            _positionalChild->setCollator(collator);
        }
    }

    ApplyResult apply(ApplyParams applyParams,
                      UpdateNodeApplyParams updateNodeApplyParams) const final;

    UpdateNode* getChild(const std::string& field) const final;

    void setChild(std::string field, std::unique_ptr<UpdateNode> child) final;

    /**
     * Gather all update operators in the subtree rooted from this into a BSONObj in the format of
     * the update command's update parameter.
     */
    BSONObj serialize(const query_shape::SerializationOptions& opts) const;

    void produceSerializationMap(
        FieldRef* currentPath,
        std::map<std::string, std::vector<std::pair<std::string, BSONObj>>>*
            operatorOrientedUpdates,
        const query_shape::SerializationOptions& opts) const final {
        for (const auto& [pathSuffix, child] : _children) {
            FieldRef::FieldRefTempAppend tempAppend(*currentPath, pathSuffix);
            child->produceSerializationMap(currentPath, operatorOrientedUpdates, opts);
        }
        // Object nodes have a positional child that must be accounted for.
        if (_positionalChild) {
            FieldRef::FieldRefTempAppend tempAppend(*currentPath, "$");
            _positionalChild->produceSerializationMap(currentPath, operatorOrientedUpdates, opts);
        }
    }

    void acceptVisitor(UpdateNodeVisitor* visitor) final {
        visitor->visit(this);
    }

    const auto& getChildren() const {
        return _children;
    }

private:
    std::map<std::string, clonable_ptr<UpdateNode>, pathsupport::cmpPathsAndArrayIndexes> _children;
    clonable_ptr<UpdateNode> _positionalChild;

    // When calling apply() causes us to merge an element of '_children' with '_positionalChild', we
    // store the result of the merge in case we need it in a future call to apply().
    mutable stdx::unordered_map<std::string, clonable_ptr<UpdateNode>> _mergedChildrenCache;
};

}  // namespace mongo
