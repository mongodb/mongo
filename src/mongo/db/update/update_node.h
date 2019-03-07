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

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/update/log_builder.h"
#include "mongo/db/update/update_executor.h"
#include "mongo/db/update/update_node_visitor.h"
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
class UpdateNode : public UpdateExecutor {
public:
    enum class Context { kAll, kInsertOnly };
    enum class Type { Object, Array, Leaf, Replacement };

    struct UpdateNodeApplyParams {
        // The path taken through the UpdateNode tree beyond where the path existed in the document.
        // For example, if the update is {$set: {'a.b.c': 5}}, and the document is {a: {}}, then at
        // the leaf node, 'pathToCreate'="b.c".
        std::shared_ptr<FieldRef> pathToCreate = std::make_shared<FieldRef>();

        // The path through the root document to 'element', ending with the field name of 'element'.
        // For example, if the update is {$set: {'a.b.c': 5}}, and the document is {a: {}}, then at
        // the leaf node, 'pathTaken'="a".
        std::shared_ptr<FieldRef> pathTaken = std::make_shared<FieldRef>();
    };

    explicit UpdateNode(Type type, Context context = Context::kAll)
        : context(context), type(type) {}
    virtual ~UpdateNode() = default;

    virtual std::unique_ptr<UpdateNode> clone() const = 0;

    ApplyResult applyUpdate(ApplyParams applyParams) const final {
        UpdateNodeApplyParams updateNodeApplyParams;
        return apply(applyParams, updateNodeApplyParams);
    }

    virtual ApplyResult apply(ApplyParams applyParams,
                              UpdateNodeApplyParams updateNodeApplyParams) const = 0;

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

    /**
     * Produces a map of serialization components for an update. The map is indexed according to
     * operator name. The value of each map entry is a vector of operator components. These two
     * components are an operator field, which is a string representing a path, and an operator
     * value, which is a BSONObj of the arguments to the operation. 'currentPath' keeps running
     * track of the full path to the current node. Note that, although produceSerializationMap()
     * mutates its 'currentPath' FieldRef for use in recursive calls, it always restores the
     * original value before the function returns so the caller will witness no change.
     */
    virtual void produceSerializationMap(
        FieldRef* currentPath,
        std::map<std::string, std::vector<std::pair<std::string, BSONObj>>>*
            operatorOrientedUpdates) const = 0;

public:
    const Context context;
    const Type type;
};

}  // namespace mongo
