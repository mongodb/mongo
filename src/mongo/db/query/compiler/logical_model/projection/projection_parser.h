// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_ast.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace projection_ast {

/**
 * Turns a BSON-representation of a projection into a walkable tree.
 *
 * 'query' and 'queryObj' refer to the associated filter provided in a find() command.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Projection parseAndAnalyze(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const BSONObj& obj,
    const MatchExpression* query,
    const BSONObj& queryObj,
    ProjectionPolicies policies,
    bool shouldOptimize = false);

/**
 * Overload of parse() to be used when not parsing a projection from a find() command.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Projection parseAndAnalyze(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const BSONObj& obj,
    ProjectionPolicies policies,
    bool shouldOptimize = false);

/**
 * Adds a node to the projection AST rooted at 'root' to the path specified by 'path'.
 */
void addNodeAtPath(ProjectionPathASTNode* root,
                   const FieldPath& path,
                   std::unique_ptr<ASTNode> newChild);

}  // namespace projection_ast
}  // namespace mongo
