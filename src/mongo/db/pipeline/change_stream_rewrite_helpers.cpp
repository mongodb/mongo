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

#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace change_stream_rewrite {
using MatchExpressionRewrite =
    std::function<std::unique_ptr<MatchExpression>(const boost::intrusive_ptr<ExpressionContext>&,
                                                   const PathMatchExpression*,
                                                   bool /* allowInexact */)>;

using AggExpressionRewrite =
    std::function<boost::intrusive_ptr<Expression>(const boost::intrusive_ptr<ExpressionContext>&,
                                                   const ExpressionFieldPath*,
                                                   bool /* allowInexact */)>;

namespace {
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
    bool allowInexact) {
    // We should only ever see predicates on the 'operationType' field.
    tassert(5554200, "Unexpected empty path", !predicate->path().empty());
    tassert(5554201,
            str::stream() << "Unexpected predicate on " << predicate->path(),
            predicate->fieldRef()->getPart(0) == DocumentSourceChangeStream::kOperationTypeField);

    // If the query is on a subfield of operationType, it will never match.
    if (predicate->fieldRef()->numParts() > 1) {
        return std::make_unique<AlwaysFalseMatchExpression>();
    }

    static const auto kExistsTrue = Document{{"$exists", true}};
    static const auto kExistsFalse = Document{{"$exists", false}};

    // Maps the operation type to the corresponding rewritten document in the oplog format.
    static const StringMap<Document> kOpTypeRewriteMap = {
        {"insert", {{"op", "i"_sd}}},
        {"delete", {{"op", "d"_sd}}},
        {"update", {{"op", "u"_sd}, {"o._id"_sd, kExistsFalse}}},
        {"replace", {{"op", "u"_sd}, {"o._id"_sd, kExistsTrue}}},
        {"drop", {{"op", "c"_sd}, {"o.drop"_sd, kExistsTrue}}},
        {"rename", {{"op", "c"_sd}, {"o.renameCollection"_sd, kExistsTrue}}},
        {"dropDatabase", {{"op", "c"_sd}, {"o.dropDatabase"_sd, kExistsTrue}}}};

    // Helper to convert a BSONElement opType into a rewritten MatchExpression.
    auto getRewrittenOpType = [&](auto& opType) -> std::unique_ptr<MatchExpression> {
        if (BSONType::String != opType.type()) {
            return std::make_unique<AlwaysFalseMatchExpression>();
        } else if (kOpTypeRewriteMap.count(opType.str())) {
            return MatchExpressionParser::parseAndNormalize(
                kOpTypeRewriteMap.at(opType.str()).toBson(), expCtx);
        }
        return nullptr;
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

            // Add the rewritten sub-expression to the '$or' expression. Abandon the entire rewrite,
            // if any of the rewrite fails.
            for (const auto& elem : inME->getEqualities()) {
                if (auto rewrittenExpr = getRewrittenOpType(elem)) {
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
    return nullptr;
}

/**
 * Attempt to rewrite a reference to the 'operationType' field such that, when evaluated over an
 * oplog document, it produces the expected change stream value for the field.
 */
boost::intrusive_ptr<Expression> exprRewriteOperationType(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExpressionFieldPath* expr,
    bool allowInexact) {
    auto fieldPath = expr->getFieldPathWithoutCurrentPrefix();
    tassert(5920000,
            str::stream() << "Unexpected field path" << fieldPath.fullPathWithPrefix(),
            fieldPath.getFieldName(0) == DocumentSourceChangeStream::kOperationTypeField);

    // If the expression is on a subfield of operationType, it will always be missing.
    if (fieldPath.getPathLength() > 1) {
        return ExpressionConstant::create(expCtx.get(), Value());
    }

    // We intend to build a $switch statement which returns the correct change stream operationType
    // based on the contents of the oplog event. Start by enumerating the different opType cases.
    std::vector<BSONObj> opCases;

    /**
     * NOTE: the list below MUST be kept up-to-date with any newly-added user-facing change stream
     * opTypes that are derived from oplog events (as opposed to events which are generated by
     * change stream stages themselves). Internal events of type {op: 'n'} are handled separately
     * and do not need to be considered here.
     */

    // Cases for handling CRUD events.
    opCases.push_back(fromjson("{case: {$eq: ['$op', 'i']}, then: 'insert'}"));
    opCases.push_back(fromjson(
        "{case: {$and: [{$eq: ['$op', 'u']}, {$eq: ['$o._id', '$$REMOVE']}]}, then: 'update'}"));
    opCases.push_back(fromjson(
        "{case: {$and: [{$eq: ['$op', 'u']}, {$ne: ['$o._id', '$$REMOVE']}]}, then: 'replace'}"));
    opCases.push_back(fromjson("{case: {$eq: ['$op', 'd']}, then: 'delete'}"));

    // Cases for handling command events.
    opCases.push_back(fromjson("{case: {$ne: ['$op', 'c']}, then: '$$REMOVE'}"));
    opCases.push_back(fromjson("{case: {$ne: ['$o.drop', '$$REMOVE']}, then: 'drop'}"));
    opCases.push_back(
        fromjson("{case: {$ne: ['$o.dropDatabase', '$$REMOVE']}, then: 'dropDatabase'}"));
    opCases.push_back(
        fromjson("{case: {$ne: ['$o.renameCollection', '$$REMOVE']}, then: 'rename'}"));

    // The default case, if nothing matches.
    auto defaultCase = ExpressionConstant::create(expCtx.get(), Value())->serialize(false);

    // Build the final expression object...
    BSONObjBuilder exprBuilder;

    BSONObjBuilder switchBuilder(exprBuilder.subobjStart("$switch"));
    switchBuilder.append("branches", opCases);
    switchBuilder << "default" << defaultCase;
    switchBuilder.doneFast();

    auto exprObj = exprBuilder.obj();

    // ... and parse it into an Expression before returning.
    return Expression::parseExpression(expCtx.get(), exprObj, expCtx->variablesParseState);
}
/**
 * Rewrites filters on 'documentKey' in a format that can be applied directly to the oplog. Returns
 * nullptr if the predicate cannot be rewritten.
 */
std::unique_ptr<MatchExpression> matchRewriteDocumentKey(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const PathMatchExpression* predicate,
    bool allowInexact) {
    tassert(5554600, "Unexpected empty predicate path", predicate->fieldRef()->numParts() > 0);
    tassert(5554601,
            str::stream() << "Unexpected predicate path: " << predicate->path(),
            predicate->fieldRef()->getPart(0) == DocumentSourceChangeStream::kDocumentKeyField);

    // Check if the predicate's path starts with "documentKey._id". If so, then we can always
    // perform an exact rewrite. If not, because of the complexities of the 'op' == 'i' case, it's
    // impractical to try to generate a rewritten predicate that matches exactly.
    bool pathStartsWithDKId =
        (predicate->fieldRef()->numParts() >= 2 &&
         predicate->fieldRef()->getPart(1) == DocumentSourceChangeStream::kIdField);
    if (!pathStartsWithDKId && !allowInexact) {
        return nullptr;
    }

    // Helper to generate a filter on the 'op' field for the specified type. This filter will also
    // include a copy of 'predicate' with the path renamed to apply to the oplog.
    auto generateFilterForOp = [&](StringData op, const StringMap<std::string>& renameList) {
        auto renamedPredicate = predicate->shallowClone();
        static_cast<PathMatchExpression*>(renamedPredicate.get())->applyRename(renameList);

        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(std::make_unique<EqualityMatchExpression>("op"_sd, Value(op)));
        andExpr->add(std::move(renamedPredicate));
        return andExpr;
    };

    // The MatchExpression which will contain the final rewritten predicate.
    auto rewrittenPredicate = std::make_unique<OrMatchExpression>();

    // Handle update, replace and delete. The predicate path can simply be renamed.
    rewrittenPredicate->add(generateFilterForOp("u"_sd, {{"documentKey", "o2"}}));
    rewrittenPredicate->add(generateFilterForOp("d"_sd, {{"documentKey", "o"}}));

    // If the path is a subfield of 'documentKey', inserts can also be handled by renaming.
    if (predicate->fieldRef()->numParts() > 1) {
        rewrittenPredicate->add(generateFilterForOp("i"_sd, {{"documentKey", "o"}}));
        return rewrittenPredicate;
    }

    // Otherwise, we must handle the {op: "i"} case where the predicate is on the full documentKey
    // field. Create an $and filter for the insert case, and seed it with {op: "i"}. If we are
    // unable to rewrite the predicate below, this filter will simply return all insert events.
    auto insertCase = std::make_unique<AndMatchExpression>();
    insertCase->add(std::make_unique<EqualityMatchExpression>("op"_sd, Value("i"_sd)));

    // Helper to convert an equality match against 'documentKey' into a match against each subfield.
    auto makeInsertDocKeyFilterForOperand =
        [&](BSONElement rhs) -> std::unique_ptr<MatchExpression> {
        // We're comparing against the full 'documentKey' field, which is an object that has an
        // '_id' subfield and possibly other subfields. If 'rhs' is not an object or if 'rhs'
        // doesn't have an '_id' subfield, it will never match.
        if (rhs.type() != BSONType::Object ||
            !rhs.embeddedObject()[DocumentSourceChangeStream::kIdField]) {
            return std::make_unique<AlwaysFalseMatchExpression>();
        }
        // Iterate over 'rhs' and add an equality match on each subfield into the $and. Each
        // fieldname is prefixed with "o." so that it applies to the document embedded in the oplog.
        auto andExpr = std::make_unique<AndMatchExpression>();
        for (auto&& subfield : rhs.embeddedObject()) {
            andExpr->add(MatchExpressionParser::parseAndNormalize(
                BSON((str::stream() << "o." << subfield.fieldNameStringData()) << subfield),
                expCtx));
        }
        return andExpr;
    };

    // There are only a limited set of predicates that we can feasibly rewrite here.
    switch (predicate->matchType()) {
        case MatchExpression::INTERNAL_EXPR_EQ:
        case MatchExpression::EQ: {
            auto cme = static_cast<const ComparisonMatchExpressionBase*>(predicate);
            insertCase->add(makeInsertDocKeyFilterForOperand(cme->getData()));
            break;
        }
        case MatchExpression::MATCH_IN: {
            // Convert the $in into an $or with one branch for each operand. We don't need to
            // account for regex operands, since these will never match.
            auto ime = static_cast<const InMatchExpression*>(predicate);
            auto orExpr = std::make_unique<OrMatchExpression>();
            for (auto& equality : ime->getEqualities()) {
                orExpr->add(makeInsertDocKeyFilterForOperand(equality));
            }
            insertCase->add(std::move(orExpr));
            break;
        }
        case MatchExpression::EXISTS:
            // An $exists predicate will match every insert, since every insert has a documentKey.
            // Leave the filter as {op: "i"} and fall through to the default 'break' case.
        default:
            // For all other predicates, we give up and just allow all insert oplog entries to pass
            // through.
            break;
    }

    // Regardless of whether we were able to fully rewrite the {op: "i"} case or not, add the
    // 'insertCase' to produce the final rewritten documentKey predicate.
    rewrittenPredicate->add(std::move(insertCase));

    return rewrittenPredicate;
}

// Map of fields names for which a simple rename is sufficient when rewriting.
StringMap<std::string> renameRegistry = {
    {"clusterTime", "ts"}, {"lsid", "lsid"}, {"txnNumber", "txnNumber"}};

// Map of field names to corresponding MatchExpression rewrite functions.
StringMap<MatchExpressionRewrite> matchRewriteRegistry = {
    {"operationType", matchRewriteOperationType}, {"documentKey", matchRewriteDocumentKey}};

// Map of field names to corresponding agg Expression rewrite functions.
StringMap<AggExpressionRewrite> exprRewriteRegistry = {{"operationType", exprRewriteOperationType}};

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
    bool allowInexact) {
    tassert(5920001, "Expression required for rewriteAggExpressionTree", expr);

    if (auto andExpr = dynamic_cast<ExpressionAnd*>(expr.get())) {
        auto& children = andExpr->getChildren();
        auto childIt = children.begin();
        while (childIt != children.end()) {
            // If inexact rewrites are permitted and any children of an $and cannot be rewritten, we
            // can omit those children without expanding the set of rejected documents.
            if (auto rewrittenPred =
                    rewriteAggExpressionTree(expCtx, *childIt, fields, allowInexact)) {
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
            if (auto rewrittenPred =
                    rewriteAggExpressionTree(expCtx, *childIt, fields, allowInexact)) {
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
                        expCtx, *childIt, fields, false /* allowInexact */)) {
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

        if (auto rewrittenPred =
                rewriteAggExpressionTree(expCtx, notChild, fields, false /* allowInexact */)) {
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
        auto firstPath = fieldExpr->getFieldPathWithoutCurrentPrefix().getFieldName(0).toString();

        // Only attempt to rewrite paths that begin with one of the caller-requested fields.
        if (fields.find(firstPath) == fields.end()) {
            return nullptr;
        }

        // Some paths can be rewritten just by renaming the path.
        if (renameRegistry.contains(firstPath)) {
            return fieldExpr->copyWithSubstitution(renameRegistry).release();
        }

        // Other paths have custom rewrite logic.
        if (exprRewriteRegistry.contains(firstPath)) {
            return exprRewriteRegistry[firstPath](expCtx, fieldExpr, allowInexact);
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
                           expCtx, *childIt, fields, false /* allowInexact */)) {
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
    bool allowInexact) {
    tassert(5687200, "MatchExpression required for rewriteMatchExpressionTree", root);

    switch (root->matchType()) {
        case MatchExpression::AND: {
            auto rewrittenAnd = std::make_unique<AndMatchExpression>();
            for (size_t i = 0; i < root->numChildren(); ++i) {
                // If inexact rewrites are permitted and any children of an $and cannot be
                // rewritten, we can omit those children without expanding the set of rejected
                // documents.
                if (auto rewrittenPred = rewriteMatchExpressionTree(
                        expCtx, root->getChild(i), fields, allowInexact)) {
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
                        expCtx, root->getChild(i), fields, allowInexact)) {
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
                if (auto rewrittenPred = rewriteMatchExpressionTree(
                        expCtx, root->getChild(i), fields, false /* allowInexact */)) {
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
                    expCtx, root->getChild(0), fields, false /* allowInexact */)) {
                return std::make_unique<NotMatchExpression>(std::move(rewrittenPred));
            }
            return nullptr;
        }
        case MatchExpression::EXPRESSION: {
            // Agg expressions are rewritten in-place, so we must clone the expression tree.
            auto origExprVal =
                static_cast<const ExprMatchExpression*>(root)->getExpression()->serialize(false);
            auto clonedExpr = Expression::parseOperand(
                expCtx.get(), BSON("" << origExprVal).firstElement(), expCtx->variablesParseState);

            // Attempt to rewrite the aggregation expression and return a new ExprMatchExpression.
            if (auto rewrittenExpr =
                    rewriteAggExpressionTree(expCtx, clonedExpr, fields, allowInexact)) {
                return std::make_unique<ExprMatchExpression>(rewrittenExpr, expCtx);
            }
            return nullptr;
        }
        default: {
            if (auto pathME = dynamic_cast<const PathMatchExpression*>(root)) {
                tassert(5687201, "Unexpected empty path", !pathME->path().empty());
                auto firstPath = pathME->fieldRef()->getPart(0).toString();

                // Only attempt to rewrite paths that begin with one of the caller-requested fields.
                if (fields.find(firstPath) == fields.end()) {
                    return nullptr;
                }

                // Some paths can be rewritten just by renaming the path.
                if (renameRegistry.contains(firstPath)) {
                    auto renamedME = pathME->shallowClone();
                    static_cast<PathMatchExpression*>(renamedME.get())->applyRename(renameRegistry);
                    return renamedME;
                }

                // Other paths have custom rewrite logic.
                if (matchRewriteRegistry.contains(firstPath)) {
                    return matchRewriteRegistry[firstPath](expCtx, pathME, allowInexact);
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
    std::set<std::string> fields) {
    // If we get null in, we return null immediately.
    if (!userMatch) {
        return nullptr;
    }

    // If the specified 'fields' set is empty, we rewrite every possible field.
    if (fields.empty()) {
        for (auto& rename : renameRegistry) {
            fields.insert(rename.first);
        }
        for (auto& meRewrite : matchRewriteRegistry) {
            fields.insert(meRewrite.first);
        }
        for (auto& exprRewrite : exprRewriteRegistry) {
            fields.insert(exprRewrite.first);
        }
    }

    // Attempt to rewrite the tree. Predicates on unknown or unrequested fields will be discarded.
    return rewriteMatchExpressionTree(expCtx, userMatch, fields, true /* allowInexact */);
}
}  // namespace change_stream_rewrite
}  // namespace mongo
