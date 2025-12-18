/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/join/predicate_extractor.h"

#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_visitor.h"

namespace mongo::join_ordering {

namespace {

// Helper visitor to check if an expression contains any references to variables. We assume that any
// expression which references a variable is ineligible to be a single table predicate (cannot be
// pushed down into the find layer).
struct SingleTablePredicateClassifier : public SelectiveConstExpressionVisitorBase {
    using SelectiveConstExpressionVisitorBase::visit;

    void visit(const ExpressionFieldPath* expr) final {
        referencesVariables |=
            expr->isVariableReference() && Variables::isUserDefinedVariable(expr->getVariableId());
    }

    bool referencesVariables{false};
};

struct PredicateExtractor {
    PredicateExtractor(const std::vector<LetVariable>& variables) {
        for (auto&& var : variables) {
            _variables.insert(var.id);
        }
    }

    bool isReferenceToLocalCollectionField(const ExpressionFieldPath* expr) {
        if (!expr->isVariableReference()) {
            return false;
        }
        // Verify that all variables in scope are references to the local collection.
        auto id = expr->getVariableId();
        return Variables::isUserDefinedVariable(id) && _variables.contains(id);
    }

    bool isReferenceToForeignCollectionField(const ExpressionFieldPath* expr) {
        return !(expr->isVariableReference() || expr->isROOT());
    }

    // Returns true if the expression is $eq: ['$a', '$$b'] and the variable reference is refering
    // to a let variable.
    bool isExprEquijoin(const ExpressionCompare* expr) {
        if (expr->getOp() != ExpressionCompare::EQ) {
            return false;
        }
        auto left = dynamic_cast<const ExpressionFieldPath*>(expr->getChildren()[0].get());
        auto right = dynamic_cast<const ExpressionFieldPath*>(expr->getChildren()[1].get());
        if (!left || !right) {
            return false;
        }
        if (isReferenceToLocalCollectionField(right)) {
            return isReferenceToForeignCollectionField(left);
        }
        if (isReferenceToLocalCollectionField(left)) {
            return isReferenceToForeignCollectionField(right);
        }
        return false;
    }

    std::unique_ptr<MatchExpression> aggToMatchExpr(
        boost::intrusive_ptr<const Expression> aggExpr) {
        boost::intrusive_ptr<ExpressionContext> expCtx(aggExpr->getExpressionContext());
        return std::make_unique<ExprMatchExpression>(aggExpr->clone(), expCtx);
    }

    boost::optional<SplitPredicatesResult> splitJoinAndSingleCollectionPredicates(
        boost::intrusive_ptr<const Expression> aggExpr) {
        // There are three cases we need to handle for splitting agg expressions:
        // 1. ExpressionAnd: Recursively try to split children and aggregate their join and single
        // table predicates.
        // 2. ExpressionCompare: Potential join predicate expressed as {$eq: ['$a', '$$a']}.
        // 3. All other expressions: Verify it is a valid single table predicate.
        if (auto andExpr = dynamic_cast<const ExpressionAnd*>(aggExpr.get())) {
            SplitPredicatesResult result;
            Expression::ExpressionVector vec;

            for (auto&& child : andExpr->getChildren()) {
                auto childRes = splitJoinAndSingleCollectionPredicates(child);
                if (!childRes.has_value()) {
                    return boost::none;
                }
                result.joinPredicates.insert(result.joinPredicates.end(),
                                             childRes->joinPredicates.begin(),
                                             childRes->joinPredicates.end());
                if (childRes->singleTablePredicates) {
                    vec.push_back(static_cast<const ExprMatchExpression*>(
                                      childRes->singleTablePredicates.get())
                                      ->getExpression());
                }
            }

            if (vec.size() == 1) {
                result.singleTablePredicates = std::make_unique<ExprMatchExpression>(
                    std::move(vec[0]), aggExpr->getExpressionContext());
            } else if (vec.size() > 1) {
                auto andExpr = boost::intrusive_ptr<ExpressionAnd>(
                    new ExpressionAnd(aggExpr->getExpressionContext(), std::move(vec)));
                result.singleTablePredicates =
                    std::make_unique<ExprMatchExpression>(andExpr, aggExpr->getExpressionContext());
            }
            return result;
        }
        if (auto cmp = dynamic_cast<const ExpressionCompare*>(aggExpr.get())) {
            if (isExprEquijoin(cmp)) {
                return SplitPredicatesResult{
                    .joinPredicates = {aggExpr},
                };
            }
        }

        SingleTablePredicateClassifier stpc;
        stage_builder::ExpressionWalker walker{&stpc, nullptr, nullptr};
        expression_walker::walk(aggExpr.get(), &walker);
        if (stpc.referencesVariables) {
            return boost::none;
        }
        return SplitPredicatesResult{
            .singleTablePredicates = aggToMatchExpr(aggExpr),
        };
    }


    boost::optional<SplitPredicatesResult> splitJoinAndSingleCollectionPredicates(
        const MatchExpression* matchExpr) {
        // There are 4 cases we need handle for splitting MatchExpressions:
        // 1. $expr: See 'splitJoinAndSingleCollectionPredicates(Expression)'. This contains
        // potential join and single table predicates.
        // 2. $and: Recursively try to split children and aggregate their join and single table
        // predicates.
        // 3. $or, $nor and $not: Recursively try to split children. We are currently only able to
        // handle the case where children contain exclusively single table predicates.
        // 4. All other expressions: Single table predicates.
        switch (matchExpr->matchType()) {
            case MatchExpression::EXPRESSION: {
                auto expr = static_cast<const ExprMatchExpression*>(matchExpr);
                return splitJoinAndSingleCollectionPredicates(expr->getExpression());
            }
            case MatchExpression::AND: {
                std::vector<boost::intrusive_ptr<const Expression>> joinPredicates;
                auto singleTablePreds = std::make_unique<AndMatchExpression>();

                // Recursive calls to split for each child and aggregate join and residual
                // predicates.
                for (size_t i = 0; i < matchExpr->numChildren(); ++i) {
                    auto childRes = splitJoinAndSingleCollectionPredicates(matchExpr->getChild(i));
                    if (!childRes.has_value()) {
                        return boost::none;
                    }
                    joinPredicates.insert(joinPredicates.end(),
                                          childRes->joinPredicates.begin(),
                                          childRes->joinPredicates.end());
                    if (childRes->singleTablePredicates) {
                        singleTablePreds->add(std::move(childRes->singleTablePredicates));
                    }
                }

                SplitPredicatesResult result{.joinPredicates = std::move(joinPredicates)};
                if (singleTablePreds->numChildren() == 1) {
                    result.singleTablePredicates = singleTablePreds->releaseChild(0);
                } else if (singleTablePreds->numChildren() > 1) {
                    result.singleTablePredicates = std::move(singleTablePreds);
                }
                return result;
            }
            case MatchExpression::OR:
            case MatchExpression::NOR:
            case MatchExpression::NOT: {
                for (size_t i = 0; i < matchExpr->numChildren(); ++i) {
                    auto childRes = splitJoinAndSingleCollectionPredicates(matchExpr->getChild(i));
                    if (!childRes.has_value()) {
                        return boost::none;
                    }
                    // If we detect join predicates in our child, we have to bail out because we
                    // don't support conjunctive or negated join predicates.
                    if (!childRes->joinPredicates.empty()) {
                        return boost::none;
                    }
                }
                break;
            }
            default:
                break;
        }
        // If we got here, we have a MatchExpresion which is a valid single table predicate.
        return SplitPredicatesResult{.singleTablePredicates = matchExpr->clone()};
    }

    stdx::unordered_set<Variables::Id> _variables;
};
}  // namespace

boost::optional<SplitPredicatesResult> splitJoinAndSingleCollectionPredicates(
    const MatchExpression* matchExpr, const std::vector<LetVariable>& variables) {
    // Verify the let variables are suitable for extracting join predicates.
    for (auto&& variable : variables) {
        auto rhs = dynamic_cast<ExpressionFieldPath*>(variable.expression.get());
        // Bail out of attempting to split the expression for join predicates if any of the
        // following are true:
        // * RHS of the variable definition is not ExpressionFieldPath (does not reference a field
        //   of the local collection of the $lookup)
        // * RHS is ExpressionFieldPath but references another variables
        // * RHS is ExpressionFieldPath but references $$ROOT
        if (!rhs || rhs->isVariableReference() || rhs->isROOT()) {
            return boost::none;
        }
        // At this point, we have verified the RHS of the variable refers to a field in the local
        // collection, which can be used to specify a join predicate.
    }
    return PredicateExtractor{variables}.splitJoinAndSingleCollectionPredicates(matchExpr);
}

};  // namespace mongo::join_ordering
