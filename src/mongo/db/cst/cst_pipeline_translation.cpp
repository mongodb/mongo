/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include <algorithm>
#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <iterator>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/cst/c_node.h"
#include "mongo/db/cst/cst_pipeline_translation.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/db/cst/key_value.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/inclusion_projection_executor.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_trigonometric.h"
#include "mongo/db/query/projection.h"
#include "mongo/db/query/projection_ast.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/visit_helper.h"

namespace mongo::cst_pipeline_translation {
namespace {
Value translateLiteralToValue(const CNode& cst);
Value translateLiteralLeaf(const CNode& cst);
boost::intrusive_ptr<Expression> translateExpression(
    const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx);

/**
 * Walk a literal array payload and produce a Value. This function is neccesary because Aggregation
 * Expression literals are required to be collapsed into Values inside ExpressionConst but
 * uncollapsed otherwise.
 */
auto translateLiteralArrayToValue(const CNode::ArrayChildren& array) {
    auto values = std::vector<Value>{};
    static_cast<void>(
        std::transform(array.begin(), array.end(), std::back_inserter(values), [&](auto&& elem) {
            return translateLiteralToValue(elem);
        }));
    return Value{std::move(values)};
}

/**
 * Walk a literal object payload and produce a Value. This function is neccesary because Aggregation
 * Expression literals are required to be collapsed into Values inside ExpressionConst but
 * uncollapsed otherwise.
 */
auto translateLiteralObjectToValue(const CNode::ObjectChildren& object) {
    auto fields = std::vector<std::pair<StringData, Value>>{};
    static_cast<void>(
        std::transform(object.begin(), object.end(), std::back_inserter(fields), [&](auto&& field) {
            return std::pair{StringData{stdx::get<UserFieldname>(field.first)},
                             translateLiteralToValue(field.second)};
        }));
    return Value{Document{std::move(fields)}};
}

/**
 * Walk a purely literal CNode and produce a Value. This function is neccesary because Aggregation
 * Expression literals are required to be collapsed into Values inside ExpressionConst but
 * uncollapsed otherwise.
 */
Value translateLiteralToValue(const CNode& cst) {
    return stdx::visit(
        visit_helper::Overloaded{
            [](const CNode::ArrayChildren& array) { return translateLiteralArrayToValue(array); },
            [](const CNode::ObjectChildren& object) {
                return translateLiteralObjectToValue(object);
            },
            [&](auto&& payload) { return translateLiteralLeaf(cst); }},
        cst.payload);
}

/**
 * Walk a literal array payload and produce an ExpressionArray.
 */
auto translateLiteralArray(const CNode::ArrayChildren& array,
                           const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto expressions = std::vector<boost::intrusive_ptr<Expression>>{};
    static_cast<void>(std::transform(
        array.begin(), array.end(), std::back_inserter(expressions), [&](auto&& elem) {
            return translateExpression(elem, expCtx);
        }));
    return ExpressionArray::create(expCtx.get(), std::move(expressions));
}

/**
 * Walk a literal object payload and produce an ExpressionObject.
 */
auto translateLiteralObject(const CNode::ObjectChildren& object,
                            const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto fields = std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>>{};
    static_cast<void>(
        std::transform(object.begin(), object.end(), std::back_inserter(fields), [&](auto&& field) {
            return std::pair{std::string{stdx::get<UserFieldname>(field.first)},
                             translateExpression(field.second, expCtx)};
        }));
    return ExpressionObject::create(expCtx.get(), std::move(fields));
}

/**
 * Walk an agg function/operator object payload and produce an Expression.
 */
boost::intrusive_ptr<Expression> translateFunctionObject(
    const CNode::ObjectChildren& object, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // Constants require using Value instead of Expression to build the tree in agg.
    if (stdx::get<KeyFieldname>(object[0].first) == KeyFieldname::constExpr ||
        stdx::get<KeyFieldname>(object[0].first) == KeyFieldname::literal)
        return make_intrusive<ExpressionConstant>(
            expCtx.get(), std::move(translateLiteralToValue(object[0].second)));

    auto expressions = std::vector<boost::intrusive_ptr<Expression>>{};
    // This assumes the Expression is in array-form.
    auto&& array = object[0].second.arrayChildren();
    static_cast<void>(std::transform(
        array.begin(), array.end(), std::back_inserter(expressions), [&](auto&& elem) {
            return translateExpression(elem, expCtx);
        }));
    switch (stdx::get<KeyFieldname>(object[0].first)) {
        case KeyFieldname::add:
            return make_intrusive<ExpressionAdd>(expCtx.get(), std::move(expressions));
        case KeyFieldname::atan2:
            return make_intrusive<ExpressionArcTangent2>(expCtx.get(), std::move(expressions));
        case KeyFieldname::andExpr:
            return make_intrusive<ExpressionAnd>(expCtx.get(), std::move(expressions));
        case KeyFieldname::orExpr:
            return make_intrusive<ExpressionOr>(expCtx.get(), std::move(expressions));
        case KeyFieldname::notExpr:
            return make_intrusive<ExpressionNot>(expCtx.get(), std::move(expressions));
        default:
            MONGO_UNREACHABLE;
    }
}

/**
 * Walk a literal leaf CNode and produce an agg Value.
 */
Value translateLiteralLeaf(const CNode& cst) {
    return stdx::visit(
        visit_helper::Overloaded{
            // These are illegal since they're non-leaf.
            [](const CNode::ArrayChildren&) -> Value { MONGO_UNREACHABLE; },
            [](const CNode::ObjectChildren&) -> Value { MONGO_UNREACHABLE; },
            // These are illegal since they're non-literal.
            [](const KeyValue&) -> Value { MONGO_UNREACHABLE; },
            [](const NonZeroKey&) -> Value { MONGO_UNREACHABLE; },
            // These payloads require a special translation to DocumentValue parlance.
            [](const UserUndefined&) { return Value{BSONUndefined}; },
            [](const UserNull&) { return Value{BSONNULL}; },
            [](const UserMinKey&) { return Value{MINKEY}; },
            [](const UserMaxKey&) { return Value{MAXKEY}; },
            // The rest convert directly.
            [](auto&& payload) { return Value{payload}; }},
        cst.payload);
}

/**
 * Walk an expression CNode and produce an agg Expression.
 */
boost::intrusive_ptr<Expression> translateExpression(
    const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return stdx::visit(
        visit_helper::Overloaded{
            // When we're not inside an agg operator/function, this is a non-leaf literal.
            [&](const CNode::ArrayChildren& array) -> boost::intrusive_ptr<Expression> {
                return translateLiteralArray(array, expCtx);
            },
            // This is either a literal object or an agg operator/function.
            [&](const CNode::ObjectChildren& object) -> boost::intrusive_ptr<Expression> {
                if (!object.empty() && stdx::holds_alternative<KeyFieldname>(object[0].first))
                    return translateFunctionObject(object, expCtx);
                else
                    return translateLiteralObject(object, expCtx);
            },
            // If a key occurs outside a particular agg operator/function, it was misplaced.
            [](const KeyValue&) -> boost::intrusive_ptr<Expression> { MONGO_UNREACHABLE; },
            [](const NonZeroKey&) -> boost::intrusive_ptr<Expression> { MONGO_UNREACHABLE; },
            // Everything else is a literal leaf.
            [&](auto &&) -> boost::intrusive_ptr<Expression> {
                return ExpressionConstant::create(expCtx.get(), translateLiteralLeaf(cst));
            }},
        cst.payload);
}

/**
 * Walk a projection CNode and produce a ProjectionASTNode. Also returns whether this was an
 * inclusion (or expressive projection) or an exclusion projection.
 */
auto translateProjection(const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    using namespace projection_ast;
    // Returns whether a KeyValue indicates inclusion or exclusion.
    auto isInclusionKeyValue = [](auto&& keyValue) {
        switch (stdx::get<KeyValue>(keyValue)) {
            case KeyValue::trueKey:
                return true;
            case KeyValue::intZeroKey:
            case KeyValue::longZeroKey:
            case KeyValue::doubleZeroKey:
            case KeyValue::decimalZeroKey:
            case KeyValue::falseKey:
                return false;
            default:
                MONGO_UNREACHABLE;
        }
    };

    if (stdx::holds_alternative<NonZeroKey>(cst.payload) ||
        (stdx::holds_alternative<KeyValue>(cst.payload) && isInclusionKeyValue(cst.payload)))
        // This is an inclusion Key.
        return std::pair{std::unique_ptr<ASTNode>{std::make_unique<BooleanConstantASTNode>(true)},
                         true};
    else if (stdx::holds_alternative<KeyValue>(cst.payload) && !isInclusionKeyValue(cst.payload))
        // This is an exclusion Key.
        return std::pair{std::unique_ptr<ASTNode>{std::make_unique<BooleanConstantASTNode>(false)},
                         false};
    else
        // This is an arbitrary expression to produce a computed field (this counts as inclusion).
        return std::pair{std::unique_ptr<ASTNode>{
                             std::make_unique<ExpressionASTNode>(translateExpression(cst, expCtx))},
                         true};
}

/**
 * Walk a project stage object CNode and produce a DocumentSourceSingleDocumentTransformation.
 */
auto translateProject(const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    using namespace projection_ast;
    auto root = ProjectionPathASTNode{};
    bool sawId = false;
    bool removeId = false;
    boost::optional<bool> inclusion;

    for (auto&& [name, child] : cst.objectChildren()) {
        // Turn the CNode into a projection AST node.
        auto&& [projection, wasInclusion] = translateProjection(child, expCtx);
        // If we see a key fieldname, make sure it's _id.
        if (auto keyFieldname = stdx::get_if<KeyFieldname>(&name);
            keyFieldname && *keyFieldname == KeyFieldname::id) {
            // Keep track of whether we've ever seen _id at all.
            sawId = true;
            // Keep track of whether we will need to remove the _id field to get around an exclusion
            // projection bug in the case where it was manually included.
            removeId = wasInclusion;
            // Add node to the projection AST.
            addNodeAtPath(&root, "_id", std::move(projection));
        } else {
            // This conditional changes the status of 'inclusion' to indicate whether we're in an
            // inclusion or exclusion projection.
            // TODO SERVER-48810: Improve error message with BSON locations.
            inclusion = !inclusion ? wasInclusion
                                   : *inclusion == wasInclusion ? wasInclusion : []() -> bool {
                uasserted(4933100,
                          "$project must include only exclusion "
                          "projection or only inclusion projection");
            }();
            // Add node to the projection AST.
            addNodeAtPath(&root, stdx::get<UserFieldname>(name), std::move(projection));
        }
    }

    // If we saw any non-_id exclusions, this is an exclusion projection.
    if (inclusion && !*inclusion) {
        // If we saw an inclusion _id for an exclusion projection, we must manually remove it or
        // projection AST will turn it into an _id exclusion due to a bug.
        // TODO Fix the bug or organize this code to circumvent projection AST.
        if (removeId)
            static_cast<void>(root.removeChild("_id"));
        return DocumentSourceProject::create(
            Projection{root, ProjectType::kExclusion}, expCtx, "$project");
        // If we saw any non-_id inclusions or computed fields, this is an inclusion projectioion.
        // Also if inclusion was not determinted, it is the default.
    } else {
        // If we didn't see _id we need to add it in manually for inclusion or projection AST
        // will incorrectly assume we want it gone.
        if (!sawId)
            addNodeAtPath(&root, "_id", std::make_unique<BooleanConstantASTNode>(true));
        return DocumentSourceProject::create(
            Projection{root, ProjectType::kInclusion}, expCtx, "$project");
    }
}

/**
 * Cast a CNode payload to a UserLong.
 */
auto translateNumToLong(const CNode& cst) {
    return stdx::visit(
        visit_helper::Overloaded{
            [](const UserDouble& userDouble) {
                return (BSON("" << userDouble).firstElement()).safeNumberLong();
            },
            [](const UserInt& userInt) {
                return (BSON("" << userInt).firstElement()).safeNumberLong();
            },
            [](const UserLong& userLong) { return userLong; },
            [](auto &&) -> UserLong { MONGO_UNREACHABLE }},
        cst.payload);
}

/**
 * Walk a skip stage object CNode and produce a DocumentSourceSkip.
 */
auto translateSkip(const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    UserLong nToSkip = translateNumToLong(cst);
    return DocumentSourceSkip::create(expCtx, nToSkip);
}

/**
 * Unwrap a limit stage CNode and produce a DocumentSourceLimit.
 */
auto translateLimit(const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    UserLong limit = translateNumToLong(cst);
    return DocumentSourceLimit::create(expCtx, limit);
}

/**
 * Walk an aggregation pipeline stage object CNode and produce a DocumentSource.
 */
boost::intrusive_ptr<DocumentSource> translateSource(
    const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    switch (cst.firstKeyFieldname()) {
        case KeyFieldname::project:
            return translateProject(cst.objectChildren()[0].second, expCtx);
        case KeyFieldname::skip:
            return translateSkip(cst.objectChildren()[0].second, expCtx);
        case KeyFieldname::limit:
            return translateLimit(cst.objectChildren()[0].second, expCtx);
        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace

/**
 * Walk a pipeline array CNode and produce a Pipeline.
 */
std::unique_ptr<Pipeline, PipelineDeleter> translatePipeline(
    const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto sources = Pipeline::SourceContainer{};
    static_cast<void>(std::transform(cst.arrayChildren().begin(),
                                     cst.arrayChildren().end(),
                                     std::back_inserter(sources),
                                     [&](auto&& elem) { return translateSource(elem, expCtx); }));
    return Pipeline::create(std::move(sources), expCtx);
}

}  // namespace mongo::cst_pipeline_translation
