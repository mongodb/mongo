/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/clonable_ptr.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/update/update_internal_node.h"

#include <map>
#include <memory>
#include <string>
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
        const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>>& arrayFilters)
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
            operatorOrientedUpdates) const final {
        for (const auto& [pathSuffix, child] : _children) {
            FieldRef::FieldRefTempAppend tempAppend(*currentPath,
                                                    toArrayFilterIdentifier(pathSuffix));
            child->produceSerializationMap(currentPath, operatorOrientedUpdates);
        }
    }

    void acceptVisitor(UpdateNodeVisitor* visitor) final {
        visitor->visit(this);
    }

private:
    const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>>& _arrayFilters;
    std::map<std::string, clonable_ptr<UpdateNode>, pathsupport::cmpPathsAndArrayIndexes> _children;

    // When calling apply() causes us to merge elements of '_children', we store the result of the
    // merge in case we need it for another array element or document.
    mutable stdx::unordered_map<UpdateNode*,
                                stdx::unordered_map<UpdateNode*, clonable_ptr<UpdateNode>>>
        _mergedChildrenCache;
};

}  // namespace mongo
