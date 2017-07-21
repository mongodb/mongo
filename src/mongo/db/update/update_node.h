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
    enum class Type { Object, Array, Leaf, Replacement };

    explicit UpdateNode(Type type) : type(type) {}
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
     * Applies the update node to 'element', creating the fields in 'pathToCreate' if required by
     * the leaves (i.e. the leaves are not all $unset). 'pathTaken' is the path through the root
     * document to 'element', ending with the field name of 'element'. 'pathToCreate' is the path
     * taken through the UpdateNode tree beyond where the path existed in the document. For example,
     * if the update is {$set: {'a.b.c': 5}}, and the document is {a: {}}, then at the leaf node,
     * pathTaken="a" and pathToCreate="b.c" If there was a positional ($) element in the update
     * expression, 'matchedField' is the index of the array element that caused the query to match
     * the document. 'fromReplication' is provided because some modifiers may ignore certain errors
     * when the update is from replication. Uses the index information in 'indexData' to determine
     * whether indexes are affected. If a LogBuilder is provided, logs the update. Outputs whether
     * the operation was a no-op. Trips a uassert (which throws UserException) if the update node
     * cannot be applied to the document. If 'validateForStorage' is true, ensures that modified
     * elements do not violate depth or DBRef constraints. Ensures that no paths in 'immutablePaths'
     * are modified (though they may be created, if they do not yet exist).
     */
    virtual void apply(mutablebson::Element element,
                       FieldRef* pathToCreate,
                       FieldRef* pathTaken,
                       StringData matchedField,
                       bool fromReplication,
                       bool validateForStorage,
                       const FieldRefSet& immutablePaths,
                       const UpdateIndexData* indexData,
                       LogBuilder* logBuilder,
                       bool* indexesAffected,
                       bool* noop) const = 0;

    /**
     * Creates a new node by merging the contents of two input nodes. The semantics of the merge
     * operation depend on the types of the input nodes. When the nodes have the same type, this
     * function dispatches the merge to a createUpdateNodeByMerging implementation defined for that
     * subtype. Throws UserException with a ConflictingUpdateOperators code when the types of the
     * input nodes differ or when any of the child nodes fail to merge.
     */
    static std::unique_ptr<UpdateNode> createUpdateNodeByMerging(const UpdateNode& leftNode,
                                                                 const UpdateNode& rightNode,
                                                                 FieldRef* pathTaken);

    const Type type;
};

}  // namespace mongo
