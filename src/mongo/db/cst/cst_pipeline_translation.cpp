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
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_sample.h"
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
 * Walk an agg function/operator object payload and produce an ExpressionVector.
 */
auto transformInputExpression(const CNode::ObjectChildren& object,
                              const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto expressions = std::vector<boost::intrusive_ptr<Expression>>{};
    stdx::visit(
        visit_helper::Overloaded{
            [&](const CNode::ArrayChildren& array) {
                static_cast<void>(std::transform(
                    array.begin(), array.end(), std::back_inserter(expressions), [&](auto&& elem) {
                        return translateExpression(elem, expCtx);
                    }));
            },
            [&](const CNode::ObjectChildren& object) {
                static_cast<void>(std::transform(
                    object.begin(),
                    object.end(),
                    std::back_inserter(expressions),
                    [&](auto&& elem) { return translateExpression(elem.second, expCtx); }));
            },
            // Everything else is a literal.
            [&](auto&&) { expressions.push_back(translateExpression(object[0].second, expCtx)); }},
        object[0].second.payload);
    return expressions;
}

/**
 * Check that the order of arguments is what we expect in an input expression.
 */
bool verifyFieldnames(const std::vector<CNode::Fieldname>& expected,
                      const std::vector<std::pair<CNode::Fieldname, CNode>>& actual) {
    if (expected.size() != actual.size())
        return false;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] != actual[i].first)
            return false;
    }
    return true;
}

/**
 * Walk an agg function/operator object payload and produce an Expression.
 */
boost::intrusive_ptr<Expression> translateFunctionObject(
    const CNode::ObjectChildren& object, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // Constants require using Value instead of Expression to build the tree in agg.
    if (stdx::get<KeyFieldname>(object[0].first) == KeyFieldname::constExpr ||
        stdx::get<KeyFieldname>(object[0].first) == KeyFieldname::literal)
        return make_intrusive<ExpressionConstant>(expCtx.get(),
                                                  translateLiteralToValue(object[0].second));
    auto expressions = transformInputExpression(object, expCtx);
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
        case KeyFieldname::cmp:
            return make_intrusive<ExpressionCompare>(
                expCtx.get(), ExpressionCompare::CMP, std::move(expressions));
        case KeyFieldname::eq:
            return make_intrusive<ExpressionCompare>(
                expCtx.get(), ExpressionCompare::EQ, std::move(expressions));
        case KeyFieldname::gt:
            return make_intrusive<ExpressionCompare>(
                expCtx.get(), ExpressionCompare::GT, std::move(expressions));
        case KeyFieldname::gte:
            return make_intrusive<ExpressionCompare>(
                expCtx.get(), ExpressionCompare::GTE, std::move(expressions));
        case KeyFieldname::lt:
            return make_intrusive<ExpressionCompare>(
                expCtx.get(), ExpressionCompare::LT, std::move(expressions));
        case KeyFieldname::lte:
            return make_intrusive<ExpressionCompare>(
                expCtx.get(), ExpressionCompare::LTE, std::move(expressions));
        case KeyFieldname::ne:
            return make_intrusive<ExpressionCompare>(
                expCtx.get(), ExpressionCompare::NE, std::move(expressions));
        case KeyFieldname::convert:
            dassert(verifyFieldnames({KeyFieldname::inputArg,
                                      KeyFieldname::toArg,
                                      KeyFieldname::onErrorArg,
                                      KeyFieldname::onNullArg},
                                     object[0].second.objectChildren()));
            return make_intrusive<ExpressionConvert>(expCtx.get(),
                                                     std::move(expressions[0]),
                                                     std::move(expressions[1]),
                                                     std::move(expressions[2]),
                                                     std::move(expressions[3]));
        case KeyFieldname::toBool:
            return ExpressionConvert::create(
                expCtx.get(), std::move(expressions[0]), BSONType::Bool);
        case KeyFieldname::toDate:
            return ExpressionConvert::create(
                expCtx.get(), std::move(expressions[0]), BSONType::Date);
        case KeyFieldname::toDecimal:
            return ExpressionConvert::create(
                expCtx.get(), std::move(expressions[0]), BSONType::NumberDecimal);
        case KeyFieldname::toDouble:
            return ExpressionConvert::create(
                expCtx.get(), std::move(expressions[0]), BSONType::NumberDouble);
        case KeyFieldname::toInt:
            return ExpressionConvert::create(
                expCtx.get(), std::move(expressions[0]), BSONType::NumberInt);
        case KeyFieldname::toLong:
            return ExpressionConvert::create(
                expCtx.get(), std::move(expressions[0]), BSONType::NumberLong);
        case KeyFieldname::toObjectId:
            return ExpressionConvert::create(
                expCtx.get(), std::move(expressions[0]), BSONType::jstOID);
        case KeyFieldname::toString:
            return ExpressionConvert::create(
                expCtx.get(), std::move(expressions[0]), BSONType::String);
        case KeyFieldname::concat:
            return make_intrusive<ExpressionConcat>(expCtx.get(), std::move(expressions));
        case KeyFieldname::dateFromString:
            dassert(verifyFieldnames({KeyFieldname::dateStringArg,
                                      KeyFieldname::formatArg,
                                      KeyFieldname::timezoneArg,
                                      KeyFieldname::onErrorArg,
                                      KeyFieldname::onNullArg},
                                     object[0].second.objectChildren()));
            return make_intrusive<ExpressionDateFromString>(expCtx.get(),
                                                            std::move(expressions[0]),
                                                            std::move(expressions[1]),
                                                            std::move(expressions[2]),
                                                            std::move(expressions[3]),
                                                            std::move(expressions[4]));
        case KeyFieldname::dateToString:
            dassert(verifyFieldnames({KeyFieldname::dateArg,
                                      KeyFieldname::formatArg,
                                      KeyFieldname::timezoneArg,
                                      KeyFieldname::onNullArg},
                                     object[0].second.objectChildren()));
            return make_intrusive<ExpressionDateToString>(expCtx.get(),
                                                          std::move(expressions[0]),
                                                          std::move(expressions[1]),
                                                          std::move(expressions[2]),
                                                          std::move(expressions[3]));
        case KeyFieldname::indexOfBytes:
            return make_intrusive<ExpressionIndexOfBytes>(expCtx.get(), std::move(expressions));
        case KeyFieldname::indexOfCP:
            return make_intrusive<ExpressionIndexOfCP>(expCtx.get(), std::move(expressions));
        case KeyFieldname::replaceOne:
            dassert(verifyFieldnames(
                {KeyFieldname::inputArg, KeyFieldname::findArg, KeyFieldname::replacementArg},
                object[0].second.objectChildren()));
            return make_intrusive<ExpressionReplaceOne>(expCtx.get(),
                                                        std::move(expressions[0]),
                                                        std::move(expressions[1]),
                                                        std::move(expressions[2]));
        case KeyFieldname::replaceAll:
            dassert(verifyFieldnames(
                {KeyFieldname::inputArg, KeyFieldname::findArg, KeyFieldname::replacementArg},
                object[0].second.objectChildren()));
            return make_intrusive<ExpressionReplaceAll>(expCtx.get(),
                                                        std::move(expressions[0]),
                                                        std::move(expressions[1]),
                                                        std::move(expressions[2]));
        case KeyFieldname::regexFind:
            dassert(verifyFieldnames(
                {KeyFieldname::inputArg, KeyFieldname::regexArg, KeyFieldname::optionsArg},
                object[0].second.objectChildren()));
            return make_intrusive<ExpressionRegexFind>(expCtx.get(),
                                                       std::move(expressions[0]),
                                                       std::move(expressions[1]),
                                                       std::move(expressions[2]),
                                                       "$regexFind");
        case KeyFieldname::regexFindAll:
            dassert(verifyFieldnames(
                {KeyFieldname::inputArg, KeyFieldname::regexArg, KeyFieldname::optionsArg},
                object[0].second.objectChildren()));
            return make_intrusive<ExpressionRegexFindAll>(expCtx.get(),
                                                          std::move(expressions[0]),
                                                          std::move(expressions[1]),
                                                          std::move(expressions[2]),
                                                          "$regexFindAll");
        case KeyFieldname::regexMatch:
            dassert(verifyFieldnames(
                {KeyFieldname::inputArg, KeyFieldname::regexArg, KeyFieldname::optionsArg},
                object[0].second.objectChildren()));
            return make_intrusive<ExpressionRegexMatch>(expCtx.get(),
                                                        std::move(expressions[0]),
                                                        std::move(expressions[1]),
                                                        std::move(expressions[2]),
                                                        "$regexMatch");
        case KeyFieldname::ltrim:
            dassert(verifyFieldnames({KeyFieldname::inputArg, KeyFieldname::charsArg},
                                     object[0].second.objectChildren()));
            return make_intrusive<ExpressionTrim>(expCtx.get(),
                                                  ExpressionTrim::TrimType::kLeft,
                                                  "$ltrim",
                                                  std::move(expressions[0]),
                                                  std::move(expressions[1]));
        case KeyFieldname::rtrim:
            dassert(verifyFieldnames({KeyFieldname::inputArg, KeyFieldname::charsArg},
                                     object[0].second.objectChildren()));
            return make_intrusive<ExpressionTrim>(expCtx.get(),
                                                  ExpressionTrim::TrimType::kRight,
                                                  "$rtrim",
                                                  std::move(expressions[0]),
                                                  std::move(expressions[1]));
        case KeyFieldname::trim:
            dassert(verifyFieldnames({KeyFieldname::inputArg, KeyFieldname::charsArg},
                                     object[0].second.objectChildren()));
            return make_intrusive<ExpressionTrim>(expCtx.get(),
                                                  ExpressionTrim::TrimType::kBoth,
                                                  "$trim",
                                                  std::move(expressions[0]),
                                                  std::move(expressions[1]));
        case KeyFieldname::split:
            return make_intrusive<ExpressionSplit>(expCtx.get(), std::move(expressions));
        case KeyFieldname::strcasecmp:
            return make_intrusive<ExpressionStrcasecmp>(expCtx.get(), std::move(expressions));
        case KeyFieldname::strLenCP:
            return make_intrusive<ExpressionStrLenCP>(expCtx.get(), std::move(expressions));
        case KeyFieldname::strLenBytes:
            return make_intrusive<ExpressionStrLenBytes>(expCtx.get(), std::move(expressions));
        case KeyFieldname::substr:
        case KeyFieldname::substrBytes:
            return make_intrusive<ExpressionSubstrBytes>(expCtx.get(), std::move(expressions));
        case KeyFieldname::substrCP:
            return make_intrusive<ExpressionSubstrCP>(expCtx.get(), std::move(expressions));
        case KeyFieldname::toLower:
            return make_intrusive<ExpressionToLower>(expCtx.get(), std::move(expressions));
        case KeyFieldname::toUpper:
            return make_intrusive<ExpressionToUpper>(expCtx.get(), std::move(expressions));
        case KeyFieldname::type:
            return make_intrusive<ExpressionType>(expCtx.get(), std::move(expressions));
        case KeyFieldname::abs:
            return make_intrusive<ExpressionAbs>(expCtx.get(), std::move(expressions));
        case KeyFieldname::ceil:
            return make_intrusive<ExpressionCeil>(expCtx.get(), std::move(expressions));
        case KeyFieldname::divide:
            return make_intrusive<ExpressionDivide>(expCtx.get(), std::move(expressions));
        case KeyFieldname::exponent:
            return make_intrusive<ExpressionExp>(expCtx.get(), std::move(expressions));
        case KeyFieldname::floor:
            return make_intrusive<ExpressionFloor>(expCtx.get(), std::move(expressions));
        case KeyFieldname::ln:
            return make_intrusive<ExpressionLn>(expCtx.get(), std::move(expressions));
        case KeyFieldname::log:
            return make_intrusive<ExpressionLog>(expCtx.get(), std::move(expressions));
        case KeyFieldname::logten:
            return make_intrusive<ExpressionLog10>(expCtx.get(), std::move(expressions));
        case KeyFieldname::mod:
            return make_intrusive<ExpressionMod>(expCtx.get(), std::move(expressions));
        case KeyFieldname::multiply:
            return make_intrusive<ExpressionMultiply>(expCtx.get(), std::move(expressions));
        case KeyFieldname::pow:
            return make_intrusive<ExpressionPow>(expCtx.get(), std::move(expressions));
        case KeyFieldname::round:
            return make_intrusive<ExpressionRound>(expCtx.get(), std::move(expressions));
        case KeyFieldname::sqrt:
            return make_intrusive<ExpressionSqrt>(expCtx.get(), std::move(expressions));
        case KeyFieldname::subtract:
            return make_intrusive<ExpressionSubtract>(expCtx.get(), std::move(expressions));
        case KeyFieldname::trunc:
            return make_intrusive<ExpressionTrunc>(expCtx.get(), std::move(expressions));
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
 * Walk a projection CNode and produce a ProjectionASTNode.
 */
std::unique_ptr<projection_ast::ASTNode> translateProjection(
    const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    using namespace projection_ast;
    if (cst.isInclusionKeyValue())
        // This is an inclusion Key.
        return std::make_unique<BooleanConstantASTNode>(true);
    else if (stdx::holds_alternative<KeyValue>(cst.payload))
        // This is an exclusion Key.
        return std::make_unique<BooleanConstantASTNode>(false);
    else
        // This is an arbitrary expression to produce a computed field (this counts as inclusion).
        return std::make_unique<ExpressionASTNode>(translateExpression(cst, expCtx));
}

/**
 * Walk an inclusion project stage object CNode and produce a
 * DocumentSourceSingleDocumentTransformation.
 */
auto translateProjectInclusion(const CNode& cst,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    using namespace projection_ast;
    auto root = ProjectionPathASTNode{};

    for (auto&& [name, child] : cst.objectChildren())
        // If we see a key fieldname, make sure it's _id.
        if (CNode::fieldnameIsId(name))
            addNodeAtPath(&root, "_id", translateProjection(child, expCtx));
        else
            addNodeAtPath(
                &root, stdx::get<UserFieldname>(name), translateProjection(child, expCtx));

    // If we didn't see _id we need to add it in manually for inclusion or projection AST
    // will incorrectly assume we want it gone.
    if (!root.getChild("_id"))
        addNodeAtPath(&root, "_id", std::make_unique<BooleanConstantASTNode>(true));
    return DocumentSourceProject::create(
        Projection{root, ProjectType::kInclusion}, expCtx, "$project");
}

/**
 * Walk an exclusion project stage object CNode and produce a
 * DocumentSourceSingleDocumentTransformation.
 */
auto translateProjectExclusion(const CNode& cst,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    using namespace projection_ast;
    auto root = ProjectionPathASTNode{};

    for (auto&& [name, child] : cst.objectChildren())
        // If we see a key fieldname, make sure it's _id.
        if (CNode::fieldnameIsId(name))
            addNodeAtPath(&root, "_id", translateProjection(child, expCtx));
        else
            addNodeAtPath(
                &root, stdx::get<UserFieldname>(name), translateProjection(child, expCtx));

    // If we saw an inclusion _id for an exclusion projection, we must manually remove it or
    // projection AST will turn it into an _id exclusion due to a bug.
    // TODO SERVER-48834: Fix the bug or organize this code to circumvent projection AST.
    if (auto childPtr = root.getChild("_id"))
        if (dynamic_cast<BooleanConstantASTNode*>(childPtr)->value())
            static_cast<void>(root.removeChild("_id"));
    return DocumentSourceProject::create(
        Projection{root, ProjectType::kExclusion}, expCtx, "$project");
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
 * Unwrap a sample stage CNode and produce a DocumentSourceMatch.
 */
auto translateSample(const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return DocumentSourceSample::create(expCtx, translateNumToLong(cst.objectChildren()[0].second));
}

/**
 * Unwrap a match stage CNode and produce a DocumentSourceSample.
 */
auto translateMatch(const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // TODO SERVER-48790, Implement CST to MatchExpression/Query command translation.
    // And add corresponding tests in cst_pipeline_translation_test.cpp.
    auto placeholder = fromjson("{}");
    return DocumentSourceMatch::create(placeholder, expCtx);
}

/**
 * Walk an aggregation pipeline stage object CNode and produce a DocumentSource.
 */
boost::intrusive_ptr<DocumentSource> translateSource(
    const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    switch (cst.firstKeyFieldname()) {
        case KeyFieldname::projectInclusion:
            return translateProjectInclusion(cst.objectChildren()[0].second, expCtx);
        case KeyFieldname::projectExclusion:
            return translateProjectExclusion(cst.objectChildren()[0].second, expCtx);
        case KeyFieldname::match:
            return translateMatch(cst.objectChildren()[0].second, expCtx);
        case KeyFieldname::skip:
            return translateSkip(cst.objectChildren()[0].second, expCtx);
        case KeyFieldname::limit:
            return translateLimit(cst.objectChildren()[0].second, expCtx);
        case KeyFieldname::sample:
            return translateSample(cst.objectChildren()[0].second, expCtx);
        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace

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
            [](const KeyValue& keyValue) -> boost::intrusive_ptr<Expression> {
                switch (keyValue) {
                    // An absentKey denotes a missing optional argument to an Expression.
                    case KeyValue::absentKey:
                        return nullptr;
                    default:
                        MONGO_UNREACHABLE;
                }
            },
            [](const NonZeroKey&) -> boost::intrusive_ptr<Expression> { MONGO_UNREACHABLE; },
            // Everything else is a literal leaf.
            [&](auto &&) -> boost::intrusive_ptr<Expression> {
                return ExpressionConstant::create(expCtx.get(), translateLiteralLeaf(cst));
            }},
        cst.payload);
}

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
