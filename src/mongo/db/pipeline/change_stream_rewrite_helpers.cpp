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

namespace mongo {
namespace change_stream_rewrite {
using ChangeStreamRewriteFunction =
    std::function<std::unique_ptr<MatchExpression>(const boost::intrusive_ptr<ExpressionContext>&,
                                                   const PathMatchExpression*,
                                                   bool /* allowInexact */)>;

namespace {
std::unique_ptr<MatchExpression> rewriteOperationType(
    const boost::intrusive_ptr<ExpressionContext>&,
    const PathMatchExpression* predicate,
    bool allowInexact) {
    // Callers of this function will always supply a 'predicate' whose path begins with the
    // "operationType" field. If the path is not an exact match, then it must represent a sub-field
    // of the "operationType" component. Any such path is trivially non-matching, because
    // "operationType" is never a sub-document.
    if (predicate->path() != DocumentSourceChangeStream::kOperationTypeField) {
        return std::make_unique<AlwaysFalseMatchExpression>();
    }
    // Map of change stream to oplog. This rewrite still needs to account for other types.
    static const std::map<std::string, std::string> opTypeMap = {
        {"insert", "i"}, {"update", "u"}, {"replace", "u"}, {"delete", "d"}};
    switch (predicate->matchType()) {
        case MatchExpression::EQ:
        case MatchExpression::INTERNAL_EXPR_EQ: {
            auto eqME = static_cast<const ComparisonMatchExpressionBase*>(predicate);
            auto rhs = eqME->getData();
            if (rhs.type() != BSONType::String || !opTypeMap.count(rhs.str())) {
                return nullptr;
            }
            return std::make_unique<EqualityMatchExpression>("op", Value(opTypeMap.at(rhs.str())));
        }
        default:
            break;
    }
    return nullptr;
}

// Map of fields names for which a simple rename is sufficient when rewriting.
StringMap<std::string> renameRegistry = {
    {"clusterTime", "ts"}, {"lsid", "lsid"}, {"txnNumber", "txnNumber"}};

// Map of field names to corresponding rewrite functions.
StringMap<ChangeStreamRewriteFunction> rewriteRegistry = {{"operationType", rewriteOperationType}};

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
    bool allowInexact) {
    tassert(5687200, "MatchExpression required for rewriteMatchExpressionTree", root);

    switch (root->matchType()) {
        case MatchExpression::AND: {
            auto rewrittenAnd = std::make_unique<AndMatchExpression>();
            for (size_t i = 0; i < root->numChildren(); ++i) {
                // If inexact rewrites are permitted and any children of an $and cannot be
                // rewritten, we can omit those children without expanding the set of rejected
                // documents.
                if (auto rewrittenPred =
                        rewriteMatchExpressionTree(expCtx, root->getChild(i), allowInexact)) {
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
                if (auto rewrittenPred =
                        rewriteMatchExpressionTree(expCtx, root->getChild(i), allowInexact)) {
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
                        expCtx, root->getChild(i), false /* allowInexact */)) {
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
                    expCtx, root->getChild(0), false /* allowInexact */)) {
                return std::make_unique<NotMatchExpression>(std::move(rewrittenPred));
            }
            return nullptr;
        }
        case MatchExpression::EXPRESSION: {
            // At present, we can only include an $expr if all its expressions are renameable.
            auto exprME = static_cast<const ExprMatchExpression*>(root);
            auto dependencies = exprME->getExpression()->getDependencies();
            auto allDependenciesRenameable =
                std::all_of(dependencies.fields.begin(),
                            dependencies.fields.end(),
                            [&](auto&& field) { return renameRegistry.contains(field); });
            if (allDependenciesRenameable) {
                auto rewrittenExpr = exprME->shallowClone();
                expression::applyRenamesToExpression(rewrittenExpr.get(), renameRegistry);
                return rewrittenExpr;
            }
            return nullptr;
        }
        default: {
            if (auto pathME = dynamic_cast<const PathMatchExpression*>(root)) {
                tassert(5687201, "Unexpected empty path", !pathME->path().empty());
                auto firstPath = pathME->fieldRef()->getPart(0).toString();

                // Some paths can be rewritten just by renaming the path.
                if (renameRegistry.contains(firstPath)) {
                    auto renamedME = pathME->shallowClone();
                    static_cast<PathMatchExpression*>(renamedME.get())->applyRename(renameRegistry);
                    return renamedME;
                }

                // Other paths have custom rewrite logic.
                tassert(5687202,
                        str::stream() << "No rewrite function found for " << pathME->path(),
                        rewriteRegistry.contains(firstPath));
                return rewriteRegistry[firstPath](expCtx, pathME, allowInexact);
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
        for (auto& rewrite : rewriteRegistry) {
            fields.insert(rewrite.first);
        }
    }

    // Extract the required fields from the user's original match expression.
    auto [dsToRewrite, _] = DocumentSourceMatch::splitMatchByModifiedFields(
        make_intrusive<DocumentSourceMatch>(userMatch->shallowClone(), expCtx),
        {DocumentSource::GetModPathsReturn::Type::kAllExcept, std::move(fields), {}});

    return !dsToRewrite ? nullptr
                        : rewriteMatchExpressionTree(
                              expCtx, dsToRewrite->getMatchExpression(), true /* allowInexact */);
}
}  // namespace change_stream_rewrite
}  // namespace mongo
