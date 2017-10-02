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

#include <memory>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/update/log_builder.h"
#include "mongo/db/update_index_data.h"
#include "mongo/util/assert_util.h"

namespace mongo {

class CollatorInterface;
class FieldRef;

/**
 * Update modifier expressions are stored as a prefix tree of UpdateNodes, where two modifiers that
 * share a field path prefix share a path prefix in the tree. The prefix tree is used to enforce
 * that no update modifier's field path is a prefix of (or equal to) another update modifier's field
 * path. The root of the UpdateNode tree is always an UpdateObjectNode. The leaves are always
 * UpdateLeafNodes.
 *
 * Example: {$set: {'a.b': 5, c: 6}, $inc: {'a.c': 1}}
 *
 *                      UpdateObjectNode
 *                         a /    \ c
 *            UpdateObjectNode    SetNode: _val = 6
 *               b /    \ c
 * SetNode: _val = 5    IncNode: _val = 1
 */
class UpdateNode {
public:
    enum class Context { kAll, kInsertOnly };
    enum class Type { Object, Array, Leaf, Replacement };

    explicit UpdateNode(Type type, Context context = Context::kAll)
        : context(context), type(type) {}
    virtual ~UpdateNode() = default;

    virtual std::unique_ptr<UpdateNode> clone() const = 0;

    /**
     * Set the collation on the node and all descendants. This is a noop if no leaf nodes require a
     * collator. If setCollator() is called, it is required that the current collator of all leaf
     * nodes is the simple collator (nullptr). The collator must outlive the modifier interface.
     * This is used to override the collation after obtaining a collection lock if the update did
     * not specify a collation and the collection has a non-simple default collation.
     */
    virtual void setCollator(const CollatorInterface* collator) = 0;

    /**
     * The parameters required by UpdateNode::apply.
     */
    struct ApplyParams {
        ApplyParams(mutablebson::Element element, const FieldRefSet& immutablePaths)
            : element(element), immutablePaths(immutablePaths) {}

        // The element to update.
        mutablebson::Element element;

        // UpdateNode::apply uasserts if it modifies an immutable path.
        const FieldRefSet& immutablePaths;

        // The path taken through the UpdateNode tree beyond where the path existed in the document.
        // For example, if the update is {$set: {'a.b.c': 5}}, and the document is {a: {}}, then at
        // the leaf node, 'pathToCreate'="b.c".
        std::shared_ptr<FieldRef> pathToCreate = std::make_shared<FieldRef>();

        // The path through the root document to 'element', ending with the field name of 'element'.
        // For example, if the update is {$set: {'a.b.c': 5}}, and the document is {a: {}}, then at
        // the leaf node, 'pathTaken'="a".
        std::shared_ptr<FieldRef> pathTaken = std::make_shared<FieldRef>();

        // If there was a positional ($) element in the update expression, 'matchedField' is the
        // index of the array element that caused the query to match the document.
        StringData matchedField;

        // True if the update is being applied to a document to be inserted. $setOnInsert behaves as
        // a no-op when this flag is false.
        bool insert = false;

        // This is provided because some modifiers may ignore certain errors when the update is from
        // replication.
        bool fromOplogApplication = false;

        // If true, UpdateNode::apply ensures that modified elements do not violate depth or DBRef
        // constraints.
        bool validateForStorage = true;

        // Used to determine whether indexes are affected.
        const UpdateIndexData* indexData = nullptr;

        // If provided, UpdateNode::apply will log the update here.
        LogBuilder* logBuilder = nullptr;
    };

    /**
     * The outputs of apply().
     */
    struct ApplyResult {
        static ApplyResult noopResult() {
            ApplyResult applyResult;
            applyResult.indexesAffected = false;
            applyResult.noop = true;
            return applyResult;
        }

        bool indexesAffected = true;
        bool noop = false;
    };

    /**
     * Applies the update node to 'applyParams.element', creating the fields in
     * 'applyParams.pathToCreate' if required by the leaves (i.e. the leaves are not all $unset).
     * Returns an ApplyResult specifying whether the operation was a no-op and whether indexes are
     * affected.
     */
    virtual ApplyResult apply(ApplyParams applyParams) const = 0;

    /**
     * Creates a new node by merging the contents of two input nodes. The semantics of the merge
     * operation depend on the types of the input nodes. When the nodes have the same type, this
     * function dispatches the merge to a createUpdateNodeByMerging implementation defined for that
     * subtype. Throws AssertionException with a ConflictingUpdateOperators code when the types of
     * the input nodes differ or when any of the child nodes fail to merge.
     */
    static std::unique_ptr<UpdateNode> createUpdateNodeByMerging(const UpdateNode& leftNode,
                                                                 const UpdateNode& rightNode,
                                                                 FieldRef* pathTaken);

    const Context context;
    const Type type;
};

}  // namespace mongo
