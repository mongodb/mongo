// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/update/update_node.h"
#include "mongo/util/modules.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * A leaf node in the prefix tree of update modifier expressions, representing an update to the
 * value at the end of a path. See comment in class definition of UpdateNode for more details.
 */
class UpdateLeafNode : public UpdateNode {
public:
    explicit UpdateLeafNode(Context context = Context::kAll) : UpdateNode(Type::Leaf, context) {}

    /**
     * Initializes the leaf node with the value in 'modExpr'. 'modExpr' is a single modifier
     * expression in the update expression. For example, if the update expression is root = {$set:
     * {'a.b': 5, c: 6}, $inc: {'a.c': 1}}, then a single modifier expression would be
     * root["$set"]["a.b"]. Returns a non-OK status if the value in 'modExpr' is invalid for the
     * type of leaf node. 'modExpr' must not be empty.
     *
     * Some leaf nodes require an ExpressionContext during initialization. For example, $pull
     * requires an ExpressionContext to construct a MatchExpression that will be used for applying
     * updates across multiple documents.
     */
    virtual Status init(BSONElement modExpr,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx) = 0;

    /* Check if it would be possible to create the path at 'pathToCreate' but don't actually create
     * it. If 'element' is not an embedded object or array (e.g., we are trying to create path
     * "a.b.c" in the document {a: 1}) or 'element' is an array but the first component in
     * 'pathToCreate' is not an array index (e.g., the path "a.b.c" in the document
     * {a: [{b: 1}, {b: 2}]}), then this function throws a AssertionException with
     * ErrorCode::PathNotViable. Otherwise, this function is a no-op.
     *
     * With the exception of $unset, update modifiers that do not create nonexistent paths ($pop,
     * $pull, $pullAll) still generate an error when it is not possible to create the path.
     */
    static void checkViability(mutablebson::Element element,
                               const FieldRef& pathToCreate,
                               const FieldRef& pathTaken);
};

}  // namespace mongo
