/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/pipeline/change_stream_rewrite_helpers.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_path.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <array>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/algorithm/string/replace.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

namespace mongo {
namespace change_stream_rewrite {
using MatchExpressionRewrite =
    std::function<std::unique_ptr<MatchExpression>(const boost::intrusive_ptr<ExpressionContext>&,
                                                   const PathMatchExpression*,
                                                   bool /* allowInexact */,
                                                   std::vector<BSONObj>&)>;

using AggExpressionRewrite =
    std::function<boost::intrusive_ptr<Expression>(const boost::intrusive_ptr<ExpressionContext>&,
                                                   const ExpressionFieldPath*,
                                                   bool /* allowInexact */,
                                                   std::vector<BSONObj>&)>;

namespace {
const auto kExistsTrue = Document{{"$exists", true}};

// Note: there is no physical '$exists' operator that can check for field-nonexistence.
// That means the expression '{field: {$exists: false}}' is syntactic sugar and will be turned into
// the slightly less efficient expression '{$not: {field: {$exists: true}}}' by the parser.
const auto kExistsFalse = Document{{"$exists", false}};

// Maps the operation type to the corresponding rewritten document in the oplog format.
const StringMap<Document> kOpTypeRewriteMap = {
    {"insert", {{"op", "i"_sd}}},
    {"delete", {{"op", "d"_sd}}},
    {"update", {{"op", "u"_sd}, {"o._id"_sd, kExistsFalse}}},
    {"replace", {{"op", "u"_sd}, {"o._id"_sd, kExistsTrue}}},
    {"drop", {{"op", "c"_sd}, {"o.drop"_sd, kExistsTrue}}},
    {"create", {{"op", "c"_sd}, {"o.create"_sd, kExistsTrue}}},
    {"createIndexes",
     {{"op", "c"_sd},
      {"$or"_sd,
       std::vector<Value>{Value({{"o.createIndexes"_sd, kExistsTrue}}),
                          Value({{"o.commitIndexBuild"_sd, kExistsTrue}})}}}},
    {"startIndexBuild", {{"op", "c"_sd}, {"o.startIndexBuild"_sd, kExistsTrue}}},
    {"abortIndexBuild", {{"op", "c"_sd}, {"o.abortIndexBuild"_sd, kExistsTrue}}},
    {"dropIndexes", {{"op", "c"_sd}, {"o.dropIndexes"_sd, kExistsTrue}}},
    {"modify", {{"op", "c"_sd}, {"o.collMod"_sd, kExistsTrue}}},
    {"rename", {{"op", "c"_sd}, {"o.renameCollection"_sd, kExistsTrue}}},
    {"dropDatabase", {{"op", "c"_sd}, {"o.dropDatabase"_sd, kExistsTrue}}}};

// $exists and null-equality checks on 'updateDescription' or its immediate subfields are AlwaysTrue
// or AlwaysFalse, since these fields are always present in the update event.
const std::set<StringData> kUpdateExistentFields = {"updateDescription",
                                                    "updateDescription.updatedFields",
                                                    "updateDescription.removedFields",
                                                    "updateDescription.truncatedArrays"};

// The oplog field corresponding to "updateDescription.updatedFields" can be in any one of three
// locations. We will attempt to construct a filter to match against them all.
const std::array<std::string, 3> kUpdateDescriptionUpdateOplogFields = {
    "o.diff.i", "o.diff.u", "o.$set"};

// The oplog field corresponding to "updateDescription.removedFields" can be in either of two
// locations. Construct an $or filter to match against them both. Because we have already validated
// that this is an equality string match, we do not need to check whether the predicate matches a
// missing field in this case.
const std::array<std::string, 2> kUpdateDescriptionRemoveOplogFields = {"o.diff.d", "o.$unset"};

const std::set<std::string> kNSValidSubFieldNames = {"ns.db", "ns.coll"};

/**
 * Whether or not a change stream event type is a CRUD operation.
 */
bool isCrudOperation(StringData name) {
    return name == "insert"_sd || name == "update"_sd || name == "replace"_sd ||
        name == "delete"_sd;
}

/**
 * Helpers to clone an expression to the same type and rename the fields to which it applies.
 */
std::unique_ptr<PathMatchExpression> cloneWithSubstitution(
    const PathMatchExpression* predicate, const StringMap<std::string>& renameList) {
    auto clonedPred = std::unique_ptr<PathMatchExpression>(
        static_cast<PathMatchExpression*>(predicate->clone().release()));
    tassert(7585302, "Failed to rename", clonedPred->applyRename(renameList));
    return clonedPred;
}
boost::intrusive_ptr<ExpressionFieldPath> cloneWithSubstitution(
    const ExpressionFieldPath* expr, const StringMap<std::string>& renameList) {
    return static_cast<ExpressionFieldPath*>(expr->copyWithSubstitution(renameList).release());
}

/**
 * Helper to resolve a predicate on a non-existent field to either AlwaysTrue or AlwaysFalse.
 */
std::unique_ptr<MatchExpression> resolvePredicateOnNonExistentField(
    const PathMatchExpression* predicate) {
    if (exec::matcher::matchesSingleElement(predicate, {})) {
        return std::make_unique<AlwaysTrueMatchExpression>();
    }
    return std::make_unique<AlwaysFalseMatchExpression>();
}

/**
 * Rewrites filters on 'operationType' in a format that can be applied directly to the oplog.
 * Returns nullptr if the predicate cannot be rewritten.
 *
 * Examples,
 *   '{operationType: "insert"}' gets rewritten to '{op: {$eq: "i"}}'
 *   '{operationType: "drop"}' gets rewritten to
 *     '{$and: [{op: {$eq: "c"}}, {o.drop: {exists: true}}]}'
 */
std::unique_ptr<MatchExpression> matchRewriteOperationType(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const PathMatchExpression* predicate,
    bool allowInexact,
    std::vector<BSONObj>& backingBsonObjs) {
    // We should only ever see predicates on the 'operationType' field.
    tassert(5554200, "Unexpected empty path", !predicate->path().empty());
    tassert(5554201,
            str::stream() << "Unexpected predicate on " << predicate->path(),
            predicate->fieldRef()->getPart(0) == DocumentSourceChangeStream::kOperationTypeField);

    // If the query is on a subfield of operationType, it will always be missing.
    if (predicate->fieldRef()->numParts() > 1) {
        return resolvePredicateOnNonExistentField(predicate);
    }

    // Helper to convert a BSONElement opType into a rewritten MatchExpression.
    auto getRewrittenOpType = [&](auto& opType) -> std::unique_ptr<MatchExpression> {
        // If the operand is not a string, then this predicate will never match. If a rewrite rule
        // does not exist for the specified operation type, then it is either handled elsewhere or
        // it's an invalid type. In either case, return $alwaysFalse so that this predicate is
        // ignored.
        if (BSONType::string != opType.type() || !kOpTypeRewriteMap.count(opType.str())) {
            return std::make_unique<AlwaysFalseMatchExpression>();
        }
        return MatchExpressionParser::parseAndNormalize(
            backingBsonObjs.emplace_back(kOpTypeRewriteMap.at(opType.str()).toBson()), expCtx);
    };

    switch (predicate->matchType()) {
        case MatchExpression::EQ:
        case MatchExpression::INTERNAL_EXPR_EQ: {
            auto eqME = static_cast<const ComparisonMatchExpressionBase*>(predicate);
            return getRewrittenOpType(eqME->getData());
        }
        case MatchExpression::MATCH_IN: {
            auto inME = static_cast<const InMatchExpression*>(predicate);

            // Regex predicates cannot be written, and rewriting only part of an '$in' would produce
            // a more restrictive filter than the original, therefore return nullptr immediately.
            if (!inME->getRegexes().empty()) {
                return nullptr;
            }

            // An empty '$in' should not match with anything, return '$alwaysFalse'.
            if (inME->getEqualities().empty()) {
                return std::make_unique<AlwaysFalseMatchExpression>();
            }

            auto rewrittenOr = std::make_unique<OrMatchExpression>();

            // First collect all requested operation types in a vector. The operation types in the
            // IN list should be sorted and unique already.
            std::vector<StringData> operationTypes;
            for (const auto& elem : inME->getEqualities()) {
                // Abandon the entire rewrite if any of the rewrites fails.
                if (BSONType::string == elem.type() &&
                    kOpTypeRewriteMap.contains(elem.valueStringData())) {
                    operationTypes.push_back(elem.valueStringData());
                }
            }

            // Sort the collection operation types so that CRUD operations come first.
            std::sort(
                operationTypes.begin(), operationTypes.end(), [&](StringData lhs, StringData rhs) {
                    bool lhsIsCrud = isCrudOperation(lhs);
                    bool rhsIsCrud = isCrudOperation(rhs);
                    if (lhsIsCrud != rhsIsCrud) {
                        // If one of the compared operations is a CRUD operation and the other
                        // isn't, sort the CRUD operation first because it should have higher
                        // selectivity.
                        return lhsIsCrud;
                    }

                    // Otherwise sort alphabetically by operation type name so the
                    return lhs < rhs;
                });

            // The individual matches for "update" and "replace" are:
            // - update:  {op: "u", "o._id": {$exists: false}}
            // - replace: {op: "u", "o._id": {$exists: true}}
            // These can be fused together into a single match:
            // - fused:   {op: "u"}
            // This also allows further optimizations so that the single op type match can be fused
            // with other op type matches into a single IN list match for the op type, e.g.
            //            {op: {$in: [...]}}
            constexpr StringData kFusedUpdateReplace = "fusedUpdateReplace"_sd;

            auto itUpdate = std::find(operationTypes.begin(), operationTypes.end(), "update");
            auto itReplace = std::find(operationTypes.begin(), operationTypes.end(), "replace");
            if (itUpdate != operationTypes.end() && itReplace != operationTypes.end()) {
                // We need to return both 'update' and 'replace' events. We will now turn 'update'
                // into a temporary 'fusedUpdateReplace' event, and remove 'replace' from the
                // vector, so we will end up only with 'fusedUpdateReplace'.
                *itUpdate = kFusedUpdateReplace;
                operationTypes.erase(itReplace);
            }

            // Add the rewritten sub-expressions to the final '$or' expression.
            for (auto&& op : operationTypes) {
                if (op == kFusedUpdateReplace) {
                    // 'updateReplace' translates to just '{op: "u"}'.
                    rewrittenOr->add(MatchExpressionParser::parseAndNormalize(
                        backingBsonObjs.emplace_back(BSON("op" << "u")), expCtx));
                } else {
                    rewrittenOr->add(MatchExpressionParser::parseAndNormalize(
                        backingBsonObjs.emplace_back(kOpTypeRewriteMap.at(op).toBson()), expCtx));
                }
            }
            return rewrittenOr;
        }
        default:
            break;
    }
    return nullptr;
}

// Helper function that produces a BSONObj with the MQL expression for rewriting the operation type.
// The BSONObj will be built only once, when the function is first called.
const BSONObj& getOperationTypeRewriteExpressionBSON() {
    // The following BSON object is constant and will be built only once, upon first entry into this
    // function.
    const static BSONObj rewriteExpression = []() -> BSONObj {
        // We intend to build a $switch statement which returns the correct change stream
        // operationType based on the contents of the oplog event. Start by enumerating the
        // different opType cases.
        std::vector<BSONObj> opCases;
        opCases.reserve(15);

        /**
         * NOTE: the list below MUST be kept up-to-date with any newly-added user-facing change
         * stream opTypes that are derived from oplog events (as opposed to events which are
         * generated by change stream stages themselves). Internal events of type {op: 'n'} are
         * handled separately and do not need to be considered here.
         */

        // Cases for handling CRUD events.
        opCases.push_back(BSON("case" << BSON("$eq" << BSON_ARRAY("$op" << "i")) << "then"
                                      << "insert"));
        opCases.push_back(BSON(
            "case" << BSON("$and" << BSON_ARRAY(BSON("$eq" << BSON_ARRAY("$op" << "u")) << BSON(
                                                    "$eq" << BSON_ARRAY("$o._id" << "$$REMOVE"))))
                   << "then"
                   << "update"));
        opCases.push_back(BSON(
            "case" << BSON("$and" << BSON_ARRAY(BSON("$eq" << BSON_ARRAY("$op" << "u")) << BSON(
                                                    "$ne" << BSON_ARRAY("$o._id" << "$$REMOVE"))))
                   << "then"
                   << "replace"));
        opCases.push_back(BSON("case" << BSON("$eq" << BSON_ARRAY("$op" << "d")) << "then"
                                      << "delete"));

        // Cases for handling command events.
        opCases.push_back(BSON("case" << BSON("$ne" << BSON_ARRAY("$op" << "c")) << "then"
                                      << "$$REMOVE"));

        constexpr std::array<std::pair<StringData, StringData>, 10> kCommandOpCases = {
            std::make_pair("$o.drop", "drop"),
            std::make_pair("$o.create", "create"),
            std::make_pair("$o.dropDatabase", "dropDatabase"),
            std::make_pair("$o.renameCollection", "rename"),
            std::make_pair("$o.createIndexes", "createIndexes"),
            std::make_pair("$o.commitIndexBuild", "createIndexes"),
            std::make_pair("$o.startIndexBuild", "startIndexBuild"),
            std::make_pair("$o.abortIndexBuild", "abortIndexBuild"),
            std::make_pair("$o.dropIndexes", "dropIndexes"),
            std::make_pair("$o.collMod", "modify"),
        };

        for (auto [oplogFieldName, opName] : kCommandOpCases) {
            opCases.push_back(BSON("case" << BSON("$ne" << BSON_ARRAY(oplogFieldName << "$$REMOVE"))
                                          << "then" << opName));
        }

        // Build the final expression object...
        BSONObjBuilder exprBuilder;

        BSONObjBuilder switchBuilder(exprBuilder.subobjStart("$switch"));
        switchBuilder.append("branches", opCases);
        // The default case, if nothing matches.
        switchBuilder << "default"
                      << "$$REMOVE";
        switchBuilder.doneFast();

        return exprBuilder.obj();
    }();

    // Return a const reference to the BSON rewrite expression object.
    return rewriteExpression;
}

/**
 * Attempt to rewrite a reference to the 'operationType' field such that, when evaluated over an
 * oplog document, it produces the expected change stream value for the field.
 */
boost::intrusive_ptr<Expression> exprRewriteOperationType(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExpressionFieldPath* expr,
    bool allowInexact,
    std::vector<BSONObj>&) {
    auto fieldPath = expr->getFieldPathWithoutCurrentPrefix();
    tassert(5920000,
            str::stream() << "Unexpected field path" << fieldPath.fullPathWithPrefix(),
            fieldPath.getFieldName(0) == DocumentSourceChangeStream::kOperationTypeField);

    // If the expression is on a subfield of operationType, it will always be missing.
    if (fieldPath.getPathLength() > 1) {
        return ExpressionConstant::create(expCtx.get(), Value());
    }

    return Expression::parseExpression(
        expCtx.get(), getOperationTypeRewriteExpressionBSON(), expCtx->variablesParseState);
}

/**
 * Rewrites filters on 'documentKey' in a format that can be applied directly to the oplog. Returns
 * nullptr if the predicate cannot be rewritten.
 */
std::unique_ptr<MatchExpression> matchRewriteDocumentKey(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const PathMatchExpression* predicate,
    bool allowInexact,
    std::vector<BSONObj>& backingBsonObjs) {
    tassert(5554600, "Unexpected empty predicate path", predicate->fieldRef()->numParts() > 0);
    tassert(5554601,
            str::stream() << "Unexpected predicate path: " << predicate->path(),
            predicate->fieldRef()->getPart(0) == DocumentSourceChangeStream::kDocumentKeyField);

    // Helper to generate a filter on the 'op' field for the specified type. This filter will also
    // include a copy of 'predicate' with the path renamed to apply to the oplog.
    auto generateFilterForOp = [&](StringData op, const StringMap<std::string>& renameList) {
        auto renamedPredicate = cloneWithSubstitution(predicate, renameList);

        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(std::make_unique<EqualityMatchExpression>("op"_sd, Value(op)));
        andExpr->add(std::move(renamedPredicate));
        return andExpr;
    };

    // The MatchExpression which will contain the final rewritten predicate.
    auto rewrittenPredicate = std::make_unique<OrMatchExpression>();

    // Handle the case of non-CRUD events. The 'documentKey' field never exists for such events, so
    // we evaluate the predicate against a non-existent field to see whether it matches.
    if (exec::matcher::matchesSingleElement(predicate, {})) {
        auto nonCRUDCase = MatchExpressionParser::parseAndNormalize(
            backingBsonObjs.emplace_back(BSON(
                "$nor" << BSON_ARRAY(BSON("op" << "i") << BSON("op" << "u") << BSON("op" << "d")))),
            expCtx);
        rewrittenPredicate->add(std::move(nonCRUDCase));
    }

    // Handle update, replace, delete and insert. The predicate path can simply be renamed.
    rewrittenPredicate->add(generateFilterForOp("u"_sd, {{"documentKey", "o2"}}));
    rewrittenPredicate->add(generateFilterForOp("d"_sd, {{"documentKey", "o"}}));
    rewrittenPredicate->add(generateFilterForOp("i"_sd, {{"documentKey", "o2"}}));

    return rewrittenPredicate;
}

/**
 * Attempt to rewrite a reference to the 'documentKey' field such that, when evaluated over an oplog
 * document, it produces the expected change stream value for the field.
 */
boost::intrusive_ptr<Expression> exprRewriteDocumentKey(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExpressionFieldPath* expr,
    bool allowInexact,
    std::vector<BSONObj>&) {
    auto fieldPath = expr->getFieldPathWithoutCurrentPrefix();
    tassert(5942300,
            str::stream() << "Unexpected field path" << fieldPath.fullPathWithPrefix(),
            fieldPath.getFieldName(0) == DocumentSourceChangeStream::kDocumentKeyField);

    // We intend to build a $switch statement which returns the correct change stream operationType
    // based on the contents of the oplog event. Start by enumerating the different opType cases.
    std::vector<BSONObj> opCases;
    opCases.reserve(2);

    // Case for 'delete'.
    auto deletePath = cloneWithSubstitution(expr, {{"documentKey", "o"}})
                          ->getFieldPathWithoutCurrentPrefix()
                          .fullPathWithPrefix();
    auto deleteFilter =
        BSON("case" << BSON("$eq" << BSON_ARRAY("$op" << "d")) << "then" << deletePath);
    opCases.push_back(std::move(deleteFilter));

    // Cases for 'insert', 'update' and 'replace'.
    auto insertUpdateAndReplacePath = cloneWithSubstitution(expr, {{"documentKey", "o2"}})
                                          ->getFieldPathWithoutCurrentPrefix()
                                          .fullPathWithPrefix();
    auto insertFilter = BSON("case" << BSON("$in" << BSON_ARRAY("$op" << BSON_ARRAY("i" << "u")))
                                    << "then" << insertUpdateAndReplacePath);
    opCases.push_back(std::move(insertFilter));

    // The default case, if nothing matches.
    auto defaultCase = ExpressionConstant::create(expCtx.get(), Value())->serialize();

    // Build the expression BSON object.
    BSONObjBuilder exprBuilder;

    BSONObjBuilder switchBuilder(exprBuilder.subobjStart("$switch"));
    switchBuilder.append("branches", opCases);
    switchBuilder << "default" << defaultCase;
    switchBuilder.doneFast();

    auto exprObj = exprBuilder.obj();

    // Parse the expression BSON object into an Expression and return the Expression.
    return Expression::parseExpression(
        expCtx.get(), std::move(exprObj), expCtx->variablesParseState);
}

/**
 * Rewrites filters on 'fullDocument' in a format that can be applied directly to the oplog. Returns
 * nullptr if the predicate cannot be rewritten.
 */
std::unique_ptr<MatchExpression> matchRewriteFullDocument(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const PathMatchExpression* predicate,
    bool allowInexact,
    std::vector<BSONObj>& backingBsonObjs) {
    tassert(5851400, "Unexpected empty predicate path", predicate->fieldRef()->numParts() > 0);
    tassert(5851401,
            str::stream() << "Unexpected predicate path: " << predicate->path(),
            predicate->fieldRef()->getPart(0) == DocumentSourceChangeStream::kFullDocumentField);

    // Because the 'fullDocument' field can be populated later in the pipeline for update events
    // (via the '{fullDocument: "updateLookup"}' option), it's impractical to try to generate a
    // rewritten predicate that matches exactly.
    if (!allowInexact) {
        return nullptr;
    }

    // For predicates on the 'fullDocument' field or a subfield thereof, we can generate a rewritten
    // predicate that matches inexactly like so:
    //   {$or: [
    //     {$and: [{op: 'u'}, {'o._id': {$exists: false}}]},
    //     {$and: [
    //       {$or: [{op: 'i'}, {op: 'u', 'o._id': {$exists: true}}]},
    //       {o: <predicate>}
    //     ]},
    //     // The following predicates are only present if the predicate matches a missing field
    //     {op: "d"},
    //     {$nor: [{op: 'i'}, {op: 'u'}, {op: 'd'}]}
    //   ]}
    auto rewrittenPredicate = std::make_unique<OrMatchExpression>();

    // Handle the case of non-replacement update entries. For the general case, we cannot apply the
    // predicate and must return all such events.
    auto updateCase = std::make_unique<AndMatchExpression>();
    updateCase->add(std::make_unique<EqualityMatchExpression>("op"_sd, Value("u"_sd)));
    updateCase->add(
        std::make_unique<NotMatchExpression>(std::make_unique<ExistsMatchExpression>("o._id"_sd)));
    rewrittenPredicate->add(std::move(updateCase));

    // Handle the case of insert and replacement entries. We can always apply the predicate in these
    // cases, because the full document is present in the oplog.
    auto insertOrReplaceCase = std::make_unique<AndMatchExpression>();

    auto insertOrReplaceOpFilter = MatchExpressionParser::parseAndNormalize(
        backingBsonObjs.emplace_back(
            BSON("$or" << BSON_ARRAY(BSON("op" << "i")
                                     << BSON("op" << "u"
                                                  << "o._id" << BSON("$exists" << true))))),
        expCtx);
    insertOrReplaceCase->add(std::move(insertOrReplaceOpFilter));

    auto predForInsertOrReplace = cloneWithSubstitution(predicate, {{"fullDocument", "o"}});
    insertOrReplaceCase->add(std::move(predForInsertOrReplace));

    rewrittenPredicate->add(std::move(insertOrReplaceCase));

    // Handle the case of delete and non-CRUD events. The 'fullDocument' field never exists for such
    // events, so we evaluate the predicate against a non-existent field to see whether it matches.
    if (exec::matcher::matchesSingleElement(predicate, {})) {
        auto deleteCase = std::make_unique<EqualityMatchExpression>("op"_sd, Value("d"_sd));
        rewrittenPredicate->add(std::move(deleteCase));

        auto nonCRUDCase = MatchExpressionParser::parseAndNormalize(
            backingBsonObjs.emplace_back(BSON(
                "$nor" << BSON_ARRAY(BSON("op" << "i") << BSON("op" << "u") << BSON("op" << "d")))),
            expCtx);
        rewrittenPredicate->add(std::move(nonCRUDCase));
    }

    return rewrittenPredicate;
}

/**
 * Rewrites filters on 'updateDescription' in a format that can be applied directly to the oplog.
 * Returns nullptr if the predicate cannot be rewritten.
 */
std::unique_ptr<MatchExpression> matchRewriteUpdateDescription(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const PathMatchExpression* predicate,
    bool allowInexact,
    std::vector<BSONObj>& backingBsonObjs) {
    tassert(5554500, "Unexpected empty predicate path", predicate->fieldRef()->numParts() > 0);
    tassert(5554501,
            str::stream() << "Unexpected predicate path: " << predicate->path(),
            predicate->fieldRef()->getPart(0) ==
                DocumentSourceChangeStream::kUpdateDescriptionField);

    // We can't determine whether we can perform a strict rewrite until we examine the predicate. We
    // wrap this in a helper function that we can call while building the filter. We try to rewrite
    // the predicate assuming that it will only be applied to non-replacement update oplog events.
    auto tryExactRewriteForUpdateEvents =
        [](const PathMatchExpression* predicate) -> std::unique_ptr<MatchExpression> {
        if (kUpdateExistentFields.count(predicate->path())) {
            // An {$exists:true} predicate will always match against any of these fields.
            if (predicate->matchType() == MatchExpression::EXISTS) {
                return std::make_unique<AlwaysTrueMatchExpression>();
            }
            // We check whether this is a ComparisonMatchExpression to ensure that the predicate is
            // type-bracketed, which means that it will *only* match missing or null. None of these
            // fields will ever be null or undefined in the change stream event.
            if (ComparisonMatchExpression::isComparisonMatchExpression(predicate) &&
                exec::matcher::matchesSingleElement(predicate, {})) {
                return std::make_unique<AlwaysFalseMatchExpression>();
            }
        }

        // For predicates on a non-dotted subfield of 'updateDescription.updatedFields' we can
        // generate a rewritten predicate that matches exactly like so:
        //
        //   {updateDescription.updatedFields.<fieldName>: <pred>}
        //     =>
        //   {$and: [
        //     {op: "u"},
        //     {"o._id": {$exists: false}},
        //     {$or: [
        //       {o.diff.i.<fieldName>: <pred>},
        //       {o.diff.u.<fieldName>: <pred>},
        //       {o.$set.<fieldName>: <pred>}
        //     ]}
        //   ]}
        if (predicate->fieldRef()->numParts() == 3 &&
            predicate->fieldRef()->getPart(1) == "updatedFields"_sd) {
            // If this predicate matches against a missing field, then we must apply an $and to all
            // three potential locations, since at least two of them will always be missing. If not,
            // then we build an $or to match if the field is present at any of the locations.
            auto rewrittenUserPredicate = [predicate]() -> std::unique_ptr<ListOfMatchExpression> {
                if (exec::matcher::matchesSingleElement(predicate, {})) {
                    return std::make_unique<AndMatchExpression>();
                }
                return std::make_unique<OrMatchExpression>();
            }();
            // Rewrite the predicate for each of the three potential oplog locations.
            // The oplog field corresponding to "updateDescription.updatedFields" can be in any one
            // of three locations. We will attempt to construct a filter to match against them all.
            for (auto&& oplogField : kUpdateDescriptionUpdateOplogFields) {
                rewrittenUserPredicate->add(cloneWithSubstitution(
                    predicate, {{"updateDescription.updatedFields", oplogField}}));
            }
            // Return the final rewritten predicate.
            return rewrittenUserPredicate;
        }

        // For $eq predicates and $in predicates on 'updateDescription.removedFields' we can
        // generate a rewritten predicate that matches exactly like so:
        //
        //   {updateDescription.removedFields: {$eq: <fieldName>}}
        //     =>
        //   {$and: [
        //     {op: "u"},
        //     {"o._id": {$exists: false}},
        //     {$or: [
        //       {o.diff.d.<fieldName>: {$exists: true}},
        //       {o.$unset.<fieldName>: {$exists: true}}
        //     ]}
        //   ]}
        //
        //   {updateDescription.removedFields: {$in: [<fieldName1>, <fieldName2>, ..]}}
        //     =>
        //   {$and: [
        //     {op: "u"},
        //     {"o._id": {$exists: false}},
        //     {$or: [
        //       {o.diff.d.<fieldName1>: {$exists: true}},
        //       {o.$unset.<fieldName1>: {$exists: true}},
        //       {o.diff.d.<fieldName2>: {$exists: true}},
        //       {o.$unset.<fieldName2>: {$exists: true}},
        //       ..
        //     ]}
        //   ]}
        if (predicate->fieldRef()->numParts() == 2 &&
            predicate->fieldRef()->getPart(1) == "removedFields"_sd) {
            // Helper to rewrite an equality on "updateDescription.removedFields" into the oplog.
            auto rewriteEqOnRemovedFields = [](auto& rhsElem) -> std::unique_ptr<MatchExpression> {
                // We can only rewrite equality matches on strings.
                if (rhsElem.type() != BSONType::string) {
                    return nullptr;
                }
                // We can only rewrite top-level fields, i.e. no dotted subpaths.
                auto fieldName = rhsElem.str();
                if (FieldRef(fieldName).numParts() > 1) {
                    return nullptr;
                }
                auto rewrittenEquality = std::make_unique<OrMatchExpression>();
                // The oplog field corresponding to "updateDescription.removedFields" can be in
                // either of two locations. Construct an $or filter to match against them both.
                // Because we have already validated that this is an equality string match, we do
                // not need to check whether the predicate matches a missing field in this case.
                for (auto&& oplogField : kUpdateDescriptionRemoveOplogFields) {
                    rewrittenEquality->add(std::make_unique<ExistsMatchExpression>(
                        StringData(fmt::format("{}.{}", oplogField, fieldName))));
                }
                return rewrittenEquality;
            };

            // We can only match against a limited number of predicates here, $eq and $in.
            switch (predicate->matchType()) {
                case MatchExpression::EQ: {
                    // Try to rewrite the predicate on "updateDescription.removedFields".
                    auto eqME = static_cast<const EqualityMatchExpression*>(predicate);
                    return rewriteEqOnRemovedFields(eqME->getData());
                }
                case MatchExpression::MATCH_IN: {
                    // If this $in includes any regexes, we can't proceed with the rewrite.
                    auto inME = static_cast<const InMatchExpression*>(predicate);
                    if (!inME->getRegexes().empty()) {
                        return nullptr;
                    }
                    // An empty '$in' should never match anything.
                    if (inME->getEqualities().empty()) {
                        return std::make_unique<AlwaysFalseMatchExpression>();
                    }
                    // Try to rewrite the $in as an $or of equalities on the oplog. If any
                    // individual rewrite fails, we must abandon the entire rewrite.
                    auto rewrittenUserPredicate = std::make_unique<OrMatchExpression>();
                    for (const auto& rhsElem : inME->getEqualities()) {
                        if (auto rewrittenEquality = rewriteEqOnRemovedFields(rhsElem)) {
                            rewrittenUserPredicate->add(std::move(rewrittenEquality));
                        } else {
                            return nullptr;
                        }
                    }
                    // Return the final rewritten predicate.
                    return rewrittenUserPredicate;
                }
                default:
                    break;
            }
        }
        // If we reach here, we cannot rewrite this predicate.
        return nullptr;
    };

    // Try to rewrite the user predicate. If we can't, then we may not be able to continue.
    auto rewrittenUserPredicate = tryExactRewriteForUpdateEvents(predicate);

    // If a strict rewrite is required and we could not rewrite the predicate, return nullptr. We
    // also return nullptr if the predicate matches a missing field, since it is pointless to try
    // to continue; we would have to return all updates, because we don't know whether they will
    // match, and all non-updates, because they will always match.
    if (!rewrittenUserPredicate &&
        (!allowInexact || exec::matcher::matchesSingleElement(predicate, {}))) {
        return nullptr;
    }

    // If we are here, then either we were able to rewrite the predicate, or we were not but an
    // inexact rewrite is permissible. First write a predicate to check that this is an update
    // that is not a full-document replacement, i.e. {op: "u", "o._id": {$exists: false}}.
    std::unique_ptr<ListOfMatchExpression> finalPredicate = std::make_unique<AndMatchExpression>();
    finalPredicate->add(std::make_unique<EqualityMatchExpression>("op"_sd, Value("u"_sd)));
    finalPredicate->add(
        std::make_unique<NotMatchExpression>(std::make_unique<ExistsMatchExpression>("o._id"_sd)));

    // If we were able to rewrite the user predicate, add it into the final predicate.
    if (rewrittenUserPredicate) {
        finalPredicate->add(std::move(rewrittenUserPredicate));
    }

    // Handle the case of non-update events. The 'updateDescription' field never exists for these
    // events, so we evaluate the predicate against a non-existent field to see whether it matches.
    if (exec::matcher::matchesSingleElement(predicate, {})) {
        auto nonUpdateCase = MatchExpressionParser::parseAndNormalize(
            backingBsonObjs.emplace_back(
                BSON("$or" << BSON_ARRAY(BSON("op" << BSON("$ne" << "u"))
                                         << BSON("op" << "u"
                                                      << "o._id" << BSON("$exists" << true))))),
            expCtx);
        finalPredicate = std::make_unique<OrMatchExpression>(std::move(finalPredicate), nullptr);
        finalPredicate->add(std::move(nonUpdateCase));
    }

    // Finally, we return the complete rewritten predicate.
    return finalPredicate;
}

// Helper to rewrite predicates on any change stream namespace field of the form {db: "dbName",
// coll: "collName"} into the oplog.

// - By default, the rewrite is performed onto the given 'nsField' which specifies an oplog field
//   containing a complete namespace string, e.g. {ns: "dbName.collName"}.
// - If 'nsFieldIsCmdNs' is true, then 'nsField' only contains the command-namespace of the
//   database, i.e. "dbName.$cmd".
// - With 'nsFieldIsCmdNs set to true, the caller can also optionally provide 'collNameField' which
//   is the field containing the collection name. The 'collNameField' may be absent, which means
//   that the operation being rewritten has a 'db' field in the change stream event, but no 'coll'
//   field.
std::unique_ptr<MatchExpression> matchRewriteGenericNamespace(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const PathMatchExpression* predicate,
    StringData nsField,
    bool nsFieldIsCmdNs = false,
    boost::optional<StringData> collNameField = boost::none) {
    // A collection name can only be specified with 'nsFieldIsCmdNs' set to true.
    tassert(5554100,
            "Cannot specify 'collNameField' with 'nsFieldIsCmdNs' set to false",
            !(!nsFieldIsCmdNs && collNameField));

    // Performs a rewrite based on the type of argument specified in the MatchExpression.
    auto getRewrittenNamespace = [&](auto&& nsElem) -> std::unique_ptr<MatchExpression> {
        switch (nsElem.type()) {
            case BSONType::object: {
                // Handles case with full namespace object, like '{ns: {db: "db", coll: "coll"}}'.
                // There must be a single part to the field path, ie. 'ns'.
                if (predicate->fieldRef()->numParts() > 1) {
                    return std::make_unique<AlwaysFalseMatchExpression>();
                }

                // Extract the object from the RHS of the predicate.
                auto nsObj = nsElem.embeddedObject();

                // If a full namespace, or a collNameField were specified, there must be 2 fields in
                // the object, i.e. db and coll.
                if ((!nsFieldIsCmdNs || collNameField) && nsObj.nFields() != 2) {
                    return std::make_unique<AlwaysFalseMatchExpression>();
                }
                //  Otherwise, there can only be 1 field in the object, i.e. db.
                if (nsFieldIsCmdNs && !collNameField && nsObj.nFields() != 1) {
                    return std::make_unique<AlwaysFalseMatchExpression>();
                }

                // Extract the db and collection from the 'ns' object. The 'collElem' will point to
                // the eoo, if it is not present.
                BSONObjIterator iter{nsObj};
                auto dbElem = iter.next();
                auto collElem = iter.next();

                // Verify that the first field is 'db' and is of type string. We should always have
                // a db entry no matter what oplog fields we are operating on.
                if (dbElem.fieldNameStringData() != "db" || dbElem.type() != BSONType::string) {
                    return std::make_unique<AlwaysFalseMatchExpression>();
                }
                // Verify that the second field is 'coll' and is of type string, if it exists.
                if (collElem &&
                    (collElem.fieldNameStringData() != "coll" ||
                     collElem.type() != BSONType::string)) {
                    return std::make_unique<AlwaysFalseMatchExpression>();
                }

                if (nsFieldIsCmdNs) {
                    auto rewrittenPred = std::make_unique<AndMatchExpression>();
                    rewrittenPred->add(std::make_unique<EqualityMatchExpression>(
                        nsField, Value(fmt::format("{}.$cmd", dbElem.valueStringDataSafe()))));

                    if (collNameField) {
                        // If we are rewriting to a combination of cmdNs and collName, we match on
                        // both.
                        rewrittenPred->add(std::make_unique<EqualityMatchExpression>(
                            *collNameField, Value(collElem.str())));
                    }
                    return rewrittenPred;
                }

                // Otherwise, we are rewriting to a full namespace field. Convert the object's
                // subfields into an exact match on the oplog field.
                return std::make_unique<EqualityMatchExpression>(
                    nsField,
                    Value(fmt::format(
                        "{}.{}", dbElem.valueStringDataSafe(), collElem.valueStringData())));
            }
            case BSONType::string: {
                // Handles case with field path, like '{"ns.coll": "coll"}'. There must be 2 parts
                // to the field path, ie. 'ns' and '[db | coll]'.
                if (predicate->fieldRef()->numParts() != 2) {
                    return std::make_unique<AlwaysFalseMatchExpression>();
                }

                // Extract the second field and verify that it is either 'db' or 'coll'.
                auto fieldName = predicate->fieldRef()->getPart(1);
                if (fieldName != "db" && fieldName != "coll") {
                    return std::make_unique<AlwaysFalseMatchExpression>();
                }

                // If the predicate is on 'coll' but we only have a db, we will never match.
                if (fieldName == "coll" && nsFieldIsCmdNs && !collNameField) {
                    return std::make_unique<AlwaysFalseMatchExpression>();
                }

                // If the predicate is on 'db' and 'nsFieldIsCmdNs' is set to true, match the $cmd
                // namespace.
                if (nsFieldIsCmdNs && fieldName == "db") {
                    return std::make_unique<EqualityMatchExpression>(
                        nsField, Value(fmt::format("{}.$cmd", nsElem.valueStringDataSafe())));
                }
                // If the predicate is on 'coll', match the 'collNameField' if we have one.
                if (collNameField && fieldName == "coll") {
                    return std::make_unique<EqualityMatchExpression>(*collNameField,
                                                                     Value(nsElem.str()));
                }

                // Otherwise, we are rewriting this predicate to operate on a field containing the
                // full namespace. If the predicate is on 'db', match all collections in that DB. If
                // the predicate is on 'coll', match that collection in all DBs.
                auto nsRegex = [&]() {
                    if (fieldName == "db") {
                        return fmt::format(
                            "^{}\\.{}",
                            DocumentSourceChangeStream::regexEscapeNsForChangeStream(
                                nsElem.valueStringDataSafe()),
                            DocumentSourceChangeStream::resolveAllCollectionsRegex(expCtx));
                    }
                    return fmt::format("{}\\.{}$",
                                       DocumentSourceChangeStream::kRegexAllDBs,
                                       DocumentSourceChangeStream::regexEscapeNsForChangeStream(
                                           nsElem.valueStringDataSafe()));
                }();

                return std::make_unique<RegexMatchExpression>(nsField, nsRegex, "");
            }
            case BSONType::regEx: {
                // Handles case with field path having regex, like '{"ns.db": /^db$/}'. There must
                // be 2 parts to the field path, ie. 'ns' and '[db | coll]'.
                if (predicate->fieldRef()->numParts() != 2) {
                    return std::make_unique<AlwaysFalseMatchExpression>();
                }

                // Extract the second field and verify that it either 'db' or 'coll'.
                auto fieldName = predicate->fieldRef()->getPart(1);
                if (fieldName != "db" && fieldName != "coll") {
                    return std::make_unique<AlwaysFalseMatchExpression>();
                }

                // If the predicate is on 'coll' but we only have a db, we will never match.
                if (fieldName == "coll" && nsFieldIsCmdNs && !collNameField) {
                    return std::make_unique<AlwaysFalseMatchExpression>();
                }

                // Rather than attempting to rewrite the regex to apply to the oplog field, we will
                // instead write an $expr to extract the dbName or collName from the oplog field,
                // and apply the unmodified regex directly to it. First get a reference to the
                // relevant field in the oplog entry.
                const std::string exprFieldRef = "$" +
                    (fieldName == "db" ? nsField : (!nsFieldIsCmdNs ? nsField : *collNameField));

                // Wrap the field in an expression to return MISSING if the field is not a string,
                // since this expression may execute on CRUD oplog entries with clashing fieldnames.
                // We will make this available to other expressions as the variable '$$oplogField'.
                BSONObj exprOplogField = BSON(
                    "$cond" << BSON(
                        "if" << BSON("$eq" << BSON_ARRAY(BSON("$type" << exprFieldRef) << "string"))
                             << "then" << exprFieldRef << "else"
                             << "$$REMOVE"));

                // Now create an expression to extract the db or coll name from the oplog entry.
                BSONObj exprDbOrCollName = [&]() -> BSONObj {
                    // If the query is on 'coll' and we have a collName field, use it as-is.
                    if (fieldName == "coll" && collNameField) {
                        return BSON("" << "$$oplogField");
                    }

                    // Otherwise, we need to split apart a full ns string. Find the separator.
                    // Return 0 if input is null in order to prevent throwing in $substrBytes.
                    BSONObj exprDotPos =
                        BSON("$ifNull" << BSON_ARRAY(
                                 BSON("$indexOfBytes" << BSON_ARRAY("$$oplogField" << ".")) << 0));

                    // If the query is on 'db', return everything up to the separator.
                    if (fieldName == "db") {
                        return BSON("$substrBytes"
                                    << BSON_ARRAY("$$oplogField" << 0 << exprDotPos));
                    }

                    // Otherwise, the query is on 'coll'. Return everything from (separator + 1)
                    // to the end of the string.
                    return BSON(
                        "$substrBytes" << BSON_ARRAY(
                            "$$oplogField" << BSON("$add" << BSON_ARRAY(1 << exprDotPos)) << -1));
                }();

                // Convert the MatchExpression $regex into a $regexMatch on the corresponding field.
                // Backslashes must be escaped to ensure they retain their special behavior.
                const auto regex = std::string(nsElem.regex());

                BSONObj exprRegexMatch = [&]() {
                    // This is needed because 'value' can be either a BSONObj or a BSONElement, and
                    // the '<<' concatenation operator behaves differently for both.
                    auto buildRegexMatch = [&](const auto& value) {
                        return BSON("$regexMatch"
                                    << BSON("input" << value << "regex" << regex << "options"
                                                    << nsElem.regexFlags()));
                    };
                    if (exprDbOrCollName.firstElement().type() == BSONType::string) {
                        return buildRegexMatch(exprDbOrCollName.firstElement());
                    }
                    return buildRegexMatch(exprDbOrCollName);
                }();

                // Finally, wrap the regex in a $let which defines the '$$oplogField' variable.
                BSONObj exprRewrittenPredicate =
                    BSON("$let" << BSON("vars" << BSON("oplogField" << exprOplogField) << "in"
                                               << exprRegexMatch));

                // Return a new ExprMatchExpression with the rewritten $regexMatch.
                return std::make_unique<ExprMatchExpression>(
                    BSON("" << exprRewrittenPredicate).firstElement(), expCtx);
            }
            default:
                break;
        }
        return nullptr;
    };

    // It is only feasible to attempt to rewrite a limited set of predicates here.
    switch (predicate->matchType()) {
        case MatchExpression::EQ:
        case MatchExpression::INTERNAL_EXPR_EQ: {
            auto eqME = static_cast<const ComparisonMatchExpressionBase*>(predicate);
            return getRewrittenNamespace(eqME->getData());
        }
        case MatchExpression::REGEX: {
            // Create the BSON element from the regex match expression and return a rewritten match
            // expression, if possible.
            auto regME = static_cast<const RegexMatchExpression*>(predicate);
            BSONObjBuilder regexBob;
            regME->serializeToBSONTypeRegex(&regexBob);
            return getRewrittenNamespace(regexBob.obj().firstElement());
        }
        case MatchExpression::MATCH_IN: {
            auto inME = static_cast<const InMatchExpression*>(predicate);

            // An empty '$in' should not match anything.
            if (inME->getEqualities().empty() && inME->getRegexes().empty()) {
                return std::make_unique<AlwaysFalseMatchExpression>();
            }

            auto rewrittenOr = std::make_unique<OrMatchExpression>();

            // For each equality expression, add the rewritten sub-expression to the '$or'
            // expression. Abandon the entire rewrite, if any of the rewrite fails.
            for (const auto& elem : inME->getEqualities()) {
                if (auto rewrittenExpr = getRewrittenNamespace(elem)) {
                    rewrittenOr->add(std::move(rewrittenExpr));
                    continue;
                }
                return nullptr;
            }

            // For each regex expression, add the rewritten sub-expression to the '$or' expression.
            // Abandon the entire rewrite, if any of the rewrite fails.
            for (const auto& regME : inME->getRegexes()) {
                BSONObjBuilder regexBob;
                regME->serializeToBSONTypeRegex(&regexBob);
                if (auto rewrittenExpr = getRewrittenNamespace(regexBob.obj().firstElement())) {
                    rewrittenOr->add(std::move(rewrittenExpr));
                    continue;
                }
                return nullptr;
            }
            return rewrittenOr;
        }
        default:
            break;
    }

    // If we have reached here, this is a predicate which we cannot rewrite.
    return nullptr;
}

/**
 * Rewrites filters on 'ns' in a format that can be applied directly to the oplog.
 * Returns nullptr if the predicate cannot be rewritten.
 */
std::unique_ptr<MatchExpression> matchRewriteNs(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const PathMatchExpression* predicate,
    bool allowInexact,
    std::vector<BSONObj>& backingBsonObjs) {
    // We should only ever see predicates on the 'ns' field.
    tassert(5554101, "Unexpected empty path", !predicate->path().empty());
    tassert(5554102,
            str::stream() << "Unexpected predicate on " << predicate->path(),
            predicate->fieldRef()->getPart(0) == DocumentSourceChangeStream::kNamespaceField);

    /**
     * NOTE: the list below MUST be kept up-to-date with any newly-added user-facing change stream
     * opTypes that are derived from oplog events (as opposed to events which are generated by
     * change stream stages themselves). Internal events of type {op: 'n'} are handled separately
     * and do not need to be considered here.
     */

    //
    // CRUD events
    //

    // CRUD ops are rewritten to the 'ns' field that contains a full namespace string.
    auto crudNsRewrite = matchRewriteGenericNamespace(expCtx, predicate, "ns"_sd);

    // If we can't rewrite this predicate for CRUD operations, then we don't expect to be able to
    // rewrite it for any other operations either.
    if (!crudNsRewrite) {
        return nullptr;
    }

    // Create the final namespace filter for CRUD operations, i.e. {op: {$ne: 'c'}}.
    auto crudNsFilter = std::make_unique<AndMatchExpression>();
    crudNsFilter->add(MatchExpressionParser::parseAndNormalize(
        backingBsonObjs.emplace_back(BSON("op" << BSON("$ne" << "c"))), expCtx));
    crudNsFilter->add(std::move(crudNsRewrite));

    //
    // Command events
    //

    // Group together all command event cases.
    auto cmdCases = std::make_unique<OrMatchExpression>();

    // The 'rename' event is rewritten to a field that contains the full namespace string.
    auto renameNsRewrite = matchRewriteGenericNamespace(expCtx, predicate, "o.renameCollection"_sd);
    tassert(5554103, "Unexpected rewrite failure", renameNsRewrite);
    cmdCases->add(std::move(renameNsRewrite));

    // The 'drop' event is rewritten to the cmdNs in 'ns' and the collection name in 'o.drop'.
    auto dropNsRewrite = matchRewriteGenericNamespace(
        expCtx, predicate, "ns"_sd, true /* nsFieldIsCmdNs */, "o.drop"_sd);
    tassert(5554104, "Unexpected rewrite failure", dropNsRewrite);
    cmdCases->add(std::move(dropNsRewrite));

    // The 'create' event is rewritten to the cmdNs in 'ns' and the collection name in 'o.create'.
    auto createNsRewrite = matchRewriteGenericNamespace(
        expCtx, predicate, "ns"_sd, true /* nsFieldIsCmdNs */, "o.create"_sd);
    tassert(6280101, "Unexpected rewrite failure", createNsRewrite);
    cmdCases->add(std::move(createNsRewrite));

    // The 'createIndexes' event is rewritten to the cmdNs in 'ns' and the collection name in
    // 'o.createIndexes'.
    auto createIndexesNsRewrite = matchRewriteGenericNamespace(
        expCtx, predicate, "ns"_sd, true /* nsFieldIsCmdNs */, "o.createIndexes"_sd);
    tassert(6339400, "Unexpected rewrite failure", createIndexesNsRewrite);
    cmdCases->add(std::move(createIndexesNsRewrite));

    // The 'commitIndexBuild' event is rewritten to the cmdNs in 'ns' and the collection name in
    // 'o.commitIndexBuild'.
    auto commitIndexBuildNsRewrite = matchRewriteGenericNamespace(
        expCtx, predicate, "ns"_sd, true /* nsFieldIsCmdNs */, "o.commitIndexBuild"_sd);
    tassert(6339401, "Unexpected rewrite failure", commitIndexBuildNsRewrite);
    cmdCases->add(std::move(commitIndexBuildNsRewrite));

    // The 'startIndexBuild' event is rewritten to the cmdNs in 'ns' and the collection name in
    // 'o.startIndexBuild'.
    auto startIndexBuildNsRewrite = matchRewriteGenericNamespace(
        expCtx, predicate, "ns"_sd, true /* nsFieldIsCmdNs */, "o.startIndexBuild"_sd);
    tassert(9315300, "Unexpected rewrite failure", startIndexBuildNsRewrite);
    cmdCases->add(std::move(startIndexBuildNsRewrite));

    // The 'abortIndexBuild' event is rewritten to the cmdNs in 'ns' and the collection name in
    // 'o.abortIndexBuild'.
    auto abortIndexBuildNsRewrite = matchRewriteGenericNamespace(
        expCtx, predicate, "ns"_sd, true /* nsFieldIsCmdNs */, "o.abortIndexBuild"_sd);
    tassert(9315400, "Unexpected rewrite failure", abortIndexBuildNsRewrite);
    cmdCases->add(std::move(abortIndexBuildNsRewrite));

    // The 'dropIndexes' event is rewritten to the cmdNs in 'ns' and the collection name in
    // 'o.dropIndexes'.
    auto dropIndexesNsRewrite = matchRewriteGenericNamespace(
        expCtx, predicate, "ns"_sd, true /* nsFieldIsCmdNs */, "o.dropIndexes"_sd);
    tassert(6339402, "Unexpected rewrite failure", dropIndexesNsRewrite);
    cmdCases->add(std::move(dropIndexesNsRewrite));

    // The 'modify' event is rewritten to the cmdNs in 'ns' and the collection name in
    // 'o.collMod'.
    auto collModNsRewrite = matchRewriteGenericNamespace(
        expCtx, predicate, "ns"_sd, true /* nsFieldIsCmdNs */, "o.collMod"_sd);
    tassert(6316200, "Unexpected rewrite failure", collModNsRewrite);
    cmdCases->add(std::move(collModNsRewrite));

    // The 'dropDatabase' event is rewritten to the cmdNs in 'ns'. It does not have a collection
    // field.
    auto dropDbNsRewrite =
        matchRewriteGenericNamespace(expCtx, predicate, "ns"_sd, true /* nsFieldIsCmdNs */);
    tassert(5554105, "Unexpected rewrite failure", dropDbNsRewrite);
    auto andDropDbNsRewrite = std::make_unique<AndMatchExpression>(std::move(dropDbNsRewrite));
    andDropDbNsRewrite->add(
        std::make_unique<EqualityMatchExpression>("o.dropDatabase"_sd, Value(1)));
    cmdCases->add(std::move(andDropDbNsRewrite));

    // Create the final namespace filter for {op: 'c'} operations.
    auto cmdNsFilter = std::make_unique<AndMatchExpression>();
    cmdNsFilter->add(MatchExpressionParser::parseAndNormalize(
        backingBsonObjs.emplace_back(BSON("op" << "c")), expCtx));
    cmdNsFilter->add(std::move(cmdCases));

    //
    // Build final 'ns' filter
    //

    // Construct the final rewritten predicate from each of the rewrite categories.
    auto rewrittenPredicate = std::make_unique<OrMatchExpression>();
    rewrittenPredicate->add(std::move(crudNsFilter));
    rewrittenPredicate->add(std::move(cmdNsFilter));

    return rewrittenPredicate;
}

// Helper function that produces a BSONObj with the MQL expression for rewriting the collection name
// of the ns. The BSONObj will be built only once, when the function is first called.
const BSONObj& getNSCollRewriteExpressionBSON() {
    // The following BSON object is constant and will be built only once, upon first entry into this
    // function.
    const static BSONObj rewriteExpression = []() -> BSONObj {
        // Helper function to extract the collection name from a given field, using the known
        // $$dbName.
        auto getCollFromNSField = [](StringData fieldName) -> BSONObj {
            return BSON("$substrBytes" << BSON_ARRAY(
                            fieldName
                            << BSON("$add" << BSON_ARRAY(BSON("$strLenBytes" << "$$dbName") << 1))
                            << -1));
        };


        /**
         * NOTE: the list below MUST be kept up-to-date with any newly-added user-facing change
         * stream opTypes that are derived from oplog events (as opposed to events which are
         * generated by change stream stages themselves). Internal events of type {op: 'n'} are
         * handled separately and do not need to be considered here.
         */
        std::vector<BSONObj> collCases;
        collCases.reserve(12);

        // Cases for handling CRUD events.
        collCases.push_back(BSON("case"
                                 << BSON("$in" << BSON_ARRAY("$op" << BSON_ARRAY("i" << "u"
                                                                                     << "d")))
                                 << "then" << getCollFromNSField("$ns")));

        // Cases for handling command events.
        collCases.push_back(BSON("case" << BSON("$ne" << BSON_ARRAY("$op" << "c")) << "then"
                                        << "$$REMOVE"));
        collCases.push_back(
            BSON("case" << BSON("$ne" << BSON_ARRAY("$o.dropDatabase" << "$$REMOVE")) << "then"
                        << "$$REMOVE"));
        collCases.push_back(BSON("case"
                                 << BSON("$ne" << BSON_ARRAY("$o.renameCollection" << "$$REMOVE"))
                                 << "then" << getCollFromNSField("$o.renameCollection")));

        // All following commands are handled in the same way.
        constexpr std::array<StringData, 8> kCommandCases = {
            "$o.drop",
            "$o.create",
            "$o.createIndexes",
            "$o.commitIndexBuild",
            "$o.startIndexBuild",
            "$o.abortIndexBuild",
            "$o.dropIndexes",
            "$o.collMod",
        };
        for (auto collCase : kCommandCases) {
            collCases.push_back(BSON("case" << BSON("$ne" << BSON_ARRAY(collCase << "$$REMOVE"))
                                            << "then" << collCase));
        }

        // Build the collection expression object...
        BSONObjBuilder collExprBuilder;

        BSONObjBuilder switchBuilder(collExprBuilder.subobjStart("$switch"));
        switchBuilder.append("branches", collCases);
        // The default case, if nothing matches.
        switchBuilder << "default"
                      << "$$REMOVE";
        switchBuilder.doneFast();

        return collExprBuilder.obj();
    }();

    // Return a const reference to the BSON rewrite expression object.
    return rewriteExpression;
}

/**
 * Attempt to rewrite a reference to the 'ns' field such that, when evaluated over an oplog
 * document, it produces the expected change stream value for the field.
 */
boost::intrusive_ptr<Expression> exprRewriteNs(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExpressionFieldPath* expr,
    bool allowInexact,
    std::vector<BSONObj>& backingBsonObjs) {
    auto fieldPath = expr->getFieldPathWithoutCurrentPrefix();

    // This function should only be called on the 'ns' field.
    tassert(5942100,
            str::stream() << "Unexpected field path" << fieldPath.fullPathWithPrefix(),
            fieldPath.getFieldName(0) == DocumentSourceChangeStream::kNamespaceField);

    // If the field path is not 'ns', 'ns.db' or 'ns.coll', it does not exist.
    if (fieldPath.getPathLength() > 1 && !kNSValidSubFieldNames.count(fieldPath.fullPath())) {
        return ExpressionConstant::create(expCtx.get(), Value());
    }

    // Firstly, we can always extract the database name directly from the "ns" field. Create a $let
    // expression which will make '$$dbName' available to all subsequent expressions. Note that we
    // do not yet complete the 'in' part of the $let, since this depends on the exact fieldPath.
    auto buildDbNameLetExpression = [](const auto& expr) -> BSONObj {
        return BSON(
            "$let" << BSON(
                "vars" << BSON("dbName" << BSON(
                                   "$substrBytes" << BSON_ARRAY(
                                       "$ns" << 0
                                             << BSON("$indexOfBytes" << BSON_ARRAY("$ns" << ".")))))
                       << "in" << expr));
    };

    // If the expression is on "ns.db", then we can simply complete and return the $let immediately.
    if (fieldPath.getPathLength() == 2 && fieldPath.getFieldName(1) == "db") {
        return Expression::parseExpression(
            expCtx.get(),
            buildDbNameLetExpression(BSON("" << "$$dbName").firstElement()),
            expCtx->variablesParseState);
    }

    // Otherwise, we need to compute the collection name for this event. This is done with large
    // $switch MQL statement, produced in 'getNSCollRewriteExpressionBSON()'. Finally, wrap the
    // expression in the $let which defines the '$$dbName' variable, and complete the 'in' parameter
    // of the $let. If the length of the fieldPath is 1 then the field reference is '$ns' and we
    // must construct the entire 'ns' object, with both 'db' and 'coll'. Otherwise, the field is
    // '$ns.coll' and we can just return the 'collExpr' $switch we constructed above.
    BSONObj rewrittenExpr = [&]() {
        if (fieldPath.getPathLength() == 1) {
            return buildDbNameLetExpression(BSON("db" << "$$dbName"
                                                      << "coll"
                                                      << getNSCollRewriteExpressionBSON()));
        }
        return buildDbNameLetExpression(getNSCollRewriteExpressionBSON());
    }();

    // Parse the expression BSON object into an Expression and return it.
    return Expression::parseExpression(
        expCtx.get(), std::move(rewrittenExpr), expCtx->variablesParseState);
}

/**
 * Rewrites filters on 'to' in a format that can be applied directly to the oplog.
 * Returns nullptr if the predicate cannot be rewritten.
 */
std::unique_ptr<MatchExpression> matchRewriteTo(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const PathMatchExpression* predicate,
    bool allowInexact,
    std::vector<BSONObj>& backingBsonObjs) {
    // We should only ever see predicates on the 'to' field.
    tassert(5554400, "Unexpected empty path", !predicate->path().empty());
    tassert(5554401,
            str::stream() << "Unexpected predicate on " << predicate->path(),
            predicate->fieldRef()->getPart(0) == DocumentSourceChangeStream::kRenameTargetNssField);

    if (auto rewriteTo = matchRewriteGenericNamespace(expCtx, predicate, "o.to"_sd)) {
        auto andRewriteTo =
            std::make_unique<AndMatchExpression>(MatchExpressionParser::parseAndNormalize(
                backingBsonObjs.emplace_back(BSON("op" << "c")), expCtx));
        andRewriteTo->add(std::move(rewriteTo));
        return andRewriteTo;
    }
    return nullptr;
}

/**
 * Attempt to rewrite a reference to the 'to' field such that, when evaluated over an oplog
 * document, it produces the expected change stream value for the field.
 */
boost::intrusive_ptr<Expression> exprRewriteTo(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExpressionFieldPath* expr,
    bool allowInexact,
    std::vector<BSONObj>& backingBsonObjs) {
    auto fieldPath = expr->getFieldPathWithoutCurrentPrefix();

    // This function should only be called on the 'to' field.
    tassert(5942200,
            str::stream() << "Unexpected field path" << fieldPath.fullPathWithPrefix(),
            fieldPath.getFieldName(0) == DocumentSourceChangeStream::kRenameTargetNssField);

    // Create a case to verify if the 'op' type is command and '$o.to' field is present.

    // Expression to extract the collection component from the 'to' field.
    auto buildCollNameExpression = []() -> BSONObj {
        return BSON(
            "$substrBytes" << BSON_ARRAY(
                "$o.to" << BSON("$add" << BSON_ARRAY(
                                    BSON("$indexOfBytes" << BSON_ARRAY("$o.to" << ".")) << 1))
                        << -1));
    };

    // Expression to extract the db component from the 'to' field.
    auto buildDbNameExpression = []() -> BSONObj {
        return BSON("$substrBytes" << BSON_ARRAY(
                        "$o.to" << 0 << BSON("$indexOfBytes" << BSON_ARRAY("$o.to" << "."))));
    };

    auto buildCondRenameExpression = [&](const auto& expr) -> BSONObj {
        return BSON(
            "$cond" << BSON(
                "if" << BSON("$and" << BSON_ARRAY(BSON("$eq" << BSON_ARRAY("$op" << "c")) << BSON(
                                                      "$ne" << BSON_ARRAY("$o.to" << "$$REMOVE"))))
                     << "then" << expr << "else"
                     << "$$REMOVE"));
    };

    BSONObj condRename;
    if (const auto& fullPath = fieldPath.fullPath(); fullPath == "to") {
        // If there is no sub-field path, then return the full 'to' object.
        condRename = buildCondRenameExpression(
            BSON("db" << buildDbNameExpression() << "coll" << buildCollNameExpression()));
    } else if (fullPath == "to.db") {
        // If the sub-path contains 'db', then return only the 'db' component.
        condRename = buildCondRenameExpression(buildDbNameExpression());
    } else if (fullPath == "to.coll") {
        // If the sub-path contains 'coll', then return only the 'coll' component.
        condRename = buildCondRenameExpression(buildCollNameExpression());
    } else {
        // Any other field path, should match nothing.
        return ExpressionConstant::create(expCtx.get(), Value());
    }

    // Parse the expression BSON object into an Expression and return it.
    return Expression::parseExpression(
        expCtx.get(), std::move(condRename), expCtx->variablesParseState);
}

/**
 * Rewrites filters on 'fullDocumentBeforeChange' in a format that can be applied directly to the
 * oplog.
 */
std::unique_ptr<MatchExpression> matchRewriteFullDocumentBeforeChange(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const PathMatchExpression* predicate,
    bool allowInexact,
    std::vector<BSONObj>& backingBsonObjs) {
    tassert(6199800, "Unexpected empty path", !predicate->path().empty());
    tassert(6199801,
            str::stream() << "Unexpected predicate path: " << predicate->path(),
            predicate->fieldRef()->getPart(0) ==
                DocumentSourceChangeStream::kFullDocumentBeforeChangeField);

    // If this predicate matches a missing value, e.g. {$eq: null}, we cannot rewrite it. Predicates
    // such as this will match all non-update and non-delete operations, and we do not know whether
    // the post-image will be available later in the pipeline. We also cannot continue if an exact
    // rewrite is required. In both cases, return nullptr immediately.
    if (!allowInexact || exec::matcher::matchesSingleElement(predicate, {})) {
        return nullptr;
    }

    // Only an update or a delete can possibly match a predicate on fullDocumentBeforeChange.
    auto updatePred = std::make_unique<AndMatchExpression>(MatchExpressionParser::parseAndNormalize(
        backingBsonObjs.emplace_back(BSON("op" << "u")), expCtx));
    auto deletePred = std::make_unique<AndMatchExpression>(MatchExpressionParser::parseAndNormalize(
        backingBsonObjs.emplace_back(BSON("op" << "d")), expCtx));

    // If the predicate is on the _id field, we can apply it to the documentKey in the oplog.
    /* Example:
     *   '{'fullDocumentBeforeChange._id': {$lt: 3}}' gets rewritten to
     *                           {$or:[
     *                              {$and: [
     *                                  {op: {$eq: 'd'}},
     *                                  {'o._id': {$lt: 3}}
     *                              ]},
     *                              {$and: [
     *                                  {op: {$eq: 'u'}},
     *                                  {'o2._id': {$lt: 3}}
     *                              ]}
     *                           ]}
     */
    if (predicate->fieldRef()->numParts() > 1 && predicate->fieldRef()->getPart(1) == "_id") {
        updatePred->add(cloneWithSubstitution(predicate, {{"fullDocumentBeforeChange", "o2"}}));
        deletePred->add(cloneWithSubstitution(predicate, {{"fullDocumentBeforeChange", "o"}}));
    }

    // Wrap the update and delete predicates in an $or, and return the completed rewrite.
    auto finalPred = std::make_unique<OrMatchExpression>(std::move(updatePred), nullptr);
    finalPred->add(std::move(deletePred));

    return finalPred;
}

// Map of fields names for which a simple rename is sufficient when rewriting.
StringMap<std::string> renameRegistry = {
    {"clusterTime", "ts"}, {"lsid", "lsid"}, {"txnNumber", "txnNumber"}};

// Map of field names to corresponding MatchExpression rewrite functions.
StringMap<MatchExpressionRewrite> matchRewriteRegistry = {
    {"operationType", matchRewriteOperationType},
    {"documentKey", matchRewriteDocumentKey},
    {"fullDocument", matchRewriteFullDocument},
    {"fullDocumentBeforeChange", matchRewriteFullDocumentBeforeChange},
    {"updateDescription", matchRewriteUpdateDescription},
    {"ns", matchRewriteNs},
    {"to", matchRewriteTo}};

// Map of field names to corresponding agg Expression rewrite functions.
StringMap<AggExpressionRewrite> exprRewriteRegistry = {{"operationType", exprRewriteOperationType},
                                                       {"documentKey", exprRewriteDocumentKey},
                                                       {"ns", exprRewriteNs},
                                                       {"to", exprRewriteTo}};

// Traverse the Expression tree and rewrite as many of them as possible. Note that the rewrite is
// performed in-place; that is, the Expression passed into the function is mutated by it.
//
// When 'allowInexact' is true, the traversal produces a "best effort" rewrite, which rejects a
// subset of the oplog entries. The inexact filter is correct so long as the original filter remains
// in place later in the pipeline. When 'allowInexact' is false, the traversal will only return a
// filter that matches the exact same set of documents.
//
// Can return null when no acceptable rewrite is possible.
boost::intrusive_ptr<Expression> rewriteAggExpressionTree(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::intrusive_ptr<Expression> expr,
    const std::set<std::string>& fields,
    bool allowInexact,
    std::vector<BSONObj>& backingBsonObjs) {
    tassert(5920001, "Expression required for rewriteAggExpressionTree", expr);

    if (auto andExpr = dynamic_cast<ExpressionAnd*>(expr.get())) {
        auto& children = andExpr->getChildren();
        auto childIt = children.begin();
        while (childIt != children.end()) {
            // If inexact rewrites are permitted and any children of an $and cannot be rewritten, we
            // can omit those children without expanding the set of rejected documents.
            if (auto rewrittenPred = rewriteAggExpressionTree(
                    expCtx, *childIt, fields, allowInexact, backingBsonObjs)) {
                *childIt = rewrittenPred;
                ++childIt;
            } else if (allowInexact) {
                childIt = children.erase(childIt);
            } else {
                return nullptr;
            }
        }
        return andExpr;
    } else if (auto orExpr = dynamic_cast<ExpressionOr*>(expr.get())) {
        auto& children = orExpr->getChildren();
        for (auto childIt = children.begin(); childIt != children.end(); ++childIt) {
            // Dropping any children of an $or would expand the set of documents rejected by the
            // filter. There is no valid rewrite of a $or if we cannot rewrite all of its children.
            // It is, however, valid for children of an $or to be inexact.
            if (auto rewrittenPred = rewriteAggExpressionTree(
                    expCtx, *childIt, fields, allowInexact, backingBsonObjs)) {
                *childIt = rewrittenPred;
            } else {
                return nullptr;
            }
        }
        return orExpr;
    } else if (auto notExpr = dynamic_cast<ExpressionNot*>(expr.get())) {
        // Note that children of a $not _cannot_ be inexact. If predicate P rejects a _subset_
        // of documents, then {$not: P} will incorrectly reject a _superset_ of documents.
        auto& notChild = notExpr->getChildren()[0];

        // A $or that is a direct child of a $not gets special treatment as a "nor" expression. If
        // inexact rewrites are permitted and any children of a "nor" cannot be rewritten, we can
        // omit those children without expanding the set of rejected documents.
        if (auto norExpr = dynamic_cast<ExpressionOr*>(notChild.get())) {
            auto& norChildren = norExpr->getChildren();
            auto childIt = norChildren.begin();
            while (childIt != norChildren.end()) {
                if (auto rewrittenPred = rewriteAggExpressionTree(
                        expCtx, *childIt, fields, false /* allowInexact */, backingBsonObjs)) {
                    *childIt = rewrittenPred;
                    ++childIt;
                } else if (allowInexact) {
                    childIt = norChildren.erase(childIt);
                } else {
                    return nullptr;
                }
            }
            return notExpr;
        }

        if (auto rewrittenPred = rewriteAggExpressionTree(
                expCtx, notChild, fields, false /* allowInexact */, backingBsonObjs)) {
            notChild = rewrittenPred;
            return notExpr;
        }
        return nullptr;
    } else if (auto fieldExpr = dynamic_cast<ExpressionFieldPath*>(expr.get())) {
        // A reference to the $$ROOT object cannot be rewritten; any rewrite would need to transform
        // the oplog entry into the final change stream event. This transformation may require a
        // document lookup to populate the "fullDocument" field.
        if (fieldExpr->isROOT()) {
            return nullptr;
        }

        // The $let definition for any user-defined variable should have already been rewritten.
        if (fieldExpr->isVariableReference()) {
            return fieldExpr;
        }

        // The remaining case is a reference to a field path in the current document.
        tassert(5920002, "Unexpected empty path", fieldExpr->getFieldPath().getPathLength() > 1);
        auto firstPath = std::string{fieldExpr->getFieldPathWithoutCurrentPrefix().getFieldName(0)};

        // Only attempt to rewrite paths that begin with one of the caller-requested fields.
        if (fields.find(firstPath) == fields.end()) {
            return nullptr;
        }

        // Some paths can be rewritten just by renaming the path.
        if (renameRegistry.contains(firstPath)) {
            return cloneWithSubstitution(fieldExpr, renameRegistry);
        }

        // Other paths have custom rewrite logic.
        if (exprRewriteRegistry.contains(firstPath)) {
            return exprRewriteRegistry[firstPath](expCtx, fieldExpr, allowInexact, backingBsonObjs);
        }

        // Others cannot be rewritten at all.
        return nullptr;
    } else {
        // Although it is possible to rewrite $let expressions in general, it is not possible when
        // the expression rebinds the '$$CURRENT' variable. When '$$CURRENT' is rebound, we can no
        // longer make assumptions about the structure of the document that the expression is
        // operating on, so we can not safely rewrite it to operate on an oplog entry.
        if (auto letExpr = dynamic_cast<const ExpressionLet*>(expr.get())) {
            for (auto& binding : letExpr->getVariableMap()) {
                if (binding.second.name == "CURRENT") {
                    return nullptr;
                }
            }
        }

        // Non-logical and non-fieldPath expression. Descend agnostically through it.
        auto& children = expr->getChildren();
        for (auto childIt = children.begin(); childIt != children.end(); ++childIt) {
            if (!*childIt) {
                // Some expressions have null children, which we leave in place.
                continue;
            } else if (auto rewrittenPred = rewriteAggExpressionTree(
                           expCtx, *childIt, fields, false /* allowInexact */, backingBsonObjs)) {
                *childIt = rewrittenPred;
            } else {
                return nullptr;
            }
        }
        return expr;
    }
    MONGO_UNREACHABLE_TASSERT(5920003);
}

// Traverse the MatchExpression tree and rewrite as many predicates as possible. When 'allowInexact'
// is true, the traversal produces a "best effort" rewrite, which rejects a subset of the oplog
// entries that would later be rejected by the 'userMatch' filter. The inexact filter is correct so
// long as 'userMatch' remains in place later in the pipeline. When 'allowInexact' is false, the
// traversal will only return a filter that matches the exact same set of documents as would be
// matched by the 'userMatch' filter.
//
// Can return null when no acceptable rewrite is possible.
//
// Assumes that the 'root' MatchExpression passed in here only contains fields that have available
// rewrite or rename rules.
std::unique_ptr<MatchExpression> rewriteMatchExpressionTree(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const MatchExpression* root,
    const std::set<std::string>& fields,
    bool allowInexact,
    std::vector<BSONObj>& backingBsonObjs) {
    tassert(5687200, "MatchExpression required for rewriteMatchExpressionTree", root);

    switch (root->matchType()) {
        case MatchExpression::AND: {
            auto rewrittenAnd = std::make_unique<AndMatchExpression>();
            for (size_t i = 0; i < root->numChildren(); ++i) {
                // If inexact rewrites are permitted and any children of an $and cannot be
                // rewritten, we can omit those children without expanding the set of rejected
                // documents.
                if (auto rewrittenPred = rewriteMatchExpressionTree(
                        expCtx, root->getChild(i), fields, allowInexact, backingBsonObjs)) {
                    rewrittenAnd->add(std::move(rewrittenPred));
                } else if (!allowInexact) {
                    return nullptr;
                }
            }
            return rewrittenAnd;
        }
        case MatchExpression::OR: {
            auto rewrittenOr = std::make_unique<OrMatchExpression>();
            for (size_t i = 0; i < root->numChildren(); ++i) {
                // Dropping any children of an $or would expand the set of documents rejected by the
                // filter. There is no valid rewrite of a $or if we cannot rewrite all of its
                // children. It is, however, valid for children of an $or to be inexact.
                if (auto rewrittenPred = rewriteMatchExpressionTree(
                        expCtx, root->getChild(i), fields, allowInexact, backingBsonObjs)) {
                    rewrittenOr->add(std::move(rewrittenPred));
                } else {
                    return nullptr;
                }
            }
            return rewrittenOr;
        }
        case MatchExpression::NOR: {
            // If inexact rewrites are permitted and any children of a $nor cannot be rewritten, we
            // can omit those children without expanding the set of rejected documents. However,
            // children of a $nor can never be inexact. If predicate P rejects a _subset_ of
            // documents, then {$nor: [P]} will incorrectly reject a _superset_ of documents.
            auto rewrittenNor = std::make_unique<NorMatchExpression>();
            for (size_t i = 0; i < root->numChildren(); ++i) {
                if (auto rewrittenPred = rewriteMatchExpressionTree(expCtx,
                                                                    root->getChild(i),
                                                                    fields,
                                                                    false /* allowInexact */,
                                                                    backingBsonObjs)) {
                    rewrittenNor->add(std::move(rewrittenPred));
                } else if (!allowInexact) {
                    return nullptr;
                }
            }
            return rewrittenNor;
        }
        case MatchExpression::NOT: {
            // Note that children of a $not _cannot_ be inexact. If predicate P rejects a _subset_
            // of documents, then {$not: P} will incorrectly reject a _superset_ of documents.
            if (auto rewrittenPred = rewriteMatchExpressionTree(
                    expCtx, root->getChild(0), fields, false /* allowInexact */, backingBsonObjs)) {
                return std::make_unique<NotMatchExpression>(std::move(rewrittenPred));
            }
            return nullptr;
        }
        case MatchExpression::EXPRESSION: {
            // Agg expressions are rewritten in-place, so we must clone the expression tree.
            auto origExprVal =
                static_cast<const ExprMatchExpression*>(root)->getExpression()->serialize(
                    SerializationOptions{});
            auto clonedExpr = Expression::parseOperand(
                expCtx.get(), BSON("" << origExprVal).firstElement(), expCtx->variablesParseState);

            // Attempt to rewrite the aggregation expression and return a new ExprMatchExpression.
            if (auto rewrittenExpr = rewriteAggExpressionTree(
                    expCtx, clonedExpr, fields, allowInexact, backingBsonObjs)) {
                return std::make_unique<ExprMatchExpression>(rewrittenExpr, expCtx);
            }
            return nullptr;
        }
        default: {
            if (auto pathME = dynamic_cast<const PathMatchExpression*>(root)) {
                // Only attempt to rewrite non-empty paths.
                if (pathME->path().empty()) {
                    return nullptr;
                }

                auto firstPath = std::string{pathME->fieldRef()->getPart(0)};

                // Only attempt to rewrite paths that begin with one of the caller-requested fields.
                if (fields.find(firstPath) == fields.end()) {
                    return nullptr;
                }

                // Some paths can be rewritten just by renaming the path.
                if (renameRegistry.contains(firstPath)) {
                    return cloneWithSubstitution(pathME, renameRegistry);
                }

                // Other paths have custom rewrite logic.
                if (matchRewriteRegistry.contains(firstPath)) {
                    return matchRewriteRegistry[firstPath](
                        expCtx, pathME, allowInexact, backingBsonObjs);
                }

                // Others cannot be rewritten at all.
                return nullptr;
            }
            // We don't recognize this predicate, so we do not attempt a rewrite.
            return nullptr;
        }
    }
}
}  // namespace

std::unique_ptr<MatchExpression> rewriteFilterForFields(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const MatchExpression* userMatch,
    std::vector<BSONObj>& backingBsonObjs,
    std::set<std::string> includeFields,
    std::set<std::string> excludeFields) {
    // If we get null in, we return null immediately.
    if (!userMatch) {
        return nullptr;
    }

    // If the specified 'fields' set is empty, we rewrite every possible field.
    if (includeFields.empty()) {
        for (auto& rename : renameRegistry) {
            includeFields.insert(rename.first);
        }
        for (auto& meRewrite : matchRewriteRegistry) {
            includeFields.insert(meRewrite.first);
        }
        for (auto& exprRewrite : exprRewriteRegistry) {
            includeFields.insert(exprRewrite.first);
        }
    }

    // Remove any fields which are present in the "excludeFields" list.
    for (auto&& excludeField : excludeFields) {
        includeFields.erase(excludeField);
    }

    // Attempt to rewrite the tree. Predicates on unknown or unrequested fields will be discarded.
    return rewriteMatchExpressionTree(
        expCtx, userMatch, includeFields, true /* allowInexact */, backingBsonObjs);
}
}  // namespace change_stream_rewrite
}  // namespace mongo
