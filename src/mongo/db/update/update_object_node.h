/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <map>
#include <string>

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/update/modifier_table.h"
#include "mongo/db/update/update_internal_node.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/unordered_map.h"

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
        const CollatorInterface* collator,
        const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>>& arrayFilters,
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
        return stdx::make_unique<UpdateObjectNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final {
        for (auto&& child : _children) {
            child.second->setCollator(collator);
        }
        if (_positionalChild) {
            _positionalChild->setCollator(collator);
        }
    }

    void apply(mutablebson::Element element,
               FieldRef* pathToCreate,
               FieldRef* pathTaken,
               StringData matchedField,
               bool fromReplication,
               bool validateForStorage,
               const FieldRefSet& immutablePaths,
               const UpdateIndexData* indexData,
               LogBuilder* logBuilder,
               bool* indexesAffected,
               bool* noop) const final;

    UpdateNode* getChild(const std::string& field) const final;

    void setChild(std::string field, std::unique_ptr<UpdateNode> child) final;

private:
    std::map<std::string, clonable_ptr<UpdateNode>> _children;
    clonable_ptr<UpdateNode> _positionalChild;

    // When calling apply() causes us to merge an element of '_children' with '_positionalChild', we
    // store the result of the merge in case we need it in a future call to apply().
    mutable stdx::unordered_map<std::string, clonable_ptr<UpdateNode>> _mergedChildrenCache;
};

}  // namespace mongo
