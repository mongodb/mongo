/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"

#include "mongo/base/init.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_arity.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_internal_eq_hashed_key.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_cond.h"
#include "mongo/db/matcher/schema/expression_internal_schema_match_array_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_parameterization.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_simplifier.h"
#include "mongo/db/query/tree_walker.h"

#include <absl/container/flat_hash_map.h>

namespace mongo {
namespace {
/**
 * An ExpressionOptimizerFunc implements tree simplifications for a MatchExpression tree with a
 * specific type of MatchExpression at the root. Except for requiring a specific MatchExpression
 * subclass, an ExpressionOptimizerFunc has the same requirements and functionality as described
 * in the specification of MatchExpression::getOptimizer(std::unique_ptr<MatchExpression>).
 */
using ExpressionOptimizerFunc =
    std::function<std::unique_ptr<MatchExpression>(std::unique_ptr<MatchExpression>)>;

absl::flat_hash_map<MatchExpression::MatchType, ExpressionOptimizerFunc>
    matchExpressionOptimizersMap;

#define REGISTER_MATCH_EXPRESSION_OPTIMIZER(key, type, optimizer)             \
    MONGO_INITIALIZER_GENERAL(addToMatchExpressionOptimizersMap_##key,        \
                              ("BeginMatchExpressionOptimizersRegistration"), \
                              ("EndMatchExpressionOptimizersRegistration"))   \
    (InitializerContext*) {                                                   \
        auto existing = matchExpressionOptimizersMap.find(type);              \
        tassert(10806400,                                                     \
                str::stream() << "Duplicate match expression (" << type       \
                              << ") rewriter registered.",                    \
                existing == matchExpressionOptimizersMap.end());              \
        matchExpressionOptimizersMap[type] = std::move(optimizer);            \
    }

MONGO_INITIALIZER_GROUP(BeginMatchExpressionOptimizersRegistration,
                        ("default"),
                        ("EndMatchExpressionOptimizersRegistration"))
MONGO_INITIALIZER_GROUP(EndMatchExpressionOptimizersRegistration,
                        ("BeginMatchExpressionOptimizersRegistration"),
                        ())

/**
 * Comparator for MatchExpression nodes.  Returns an integer less than, equal to, or greater
 * than zero if 'lhs' is less than, equal to, or greater than 'rhs', respectively.
 *
 * Sorts by:
 * 1) operator type (MatchExpression::MatchType)
 * 2) path name (MatchExpression::path())
 * 3) sort order of children
 * 4) number of children (MatchExpression::numChildren())
 *
 * The third item is needed to ensure that match expression trees which should have the same
 * cache key always sort the same way. If you're wondering when the tuple (operator type, path
 * name) could ever be equal, consider this query:
 *
 * {$and:[{$or:[{a:1},{a:2}]},{$or:[{a:1},{b:2}]}]}
 *
 * The two OR nodes would compare as equal in this case were it not for tuple item #3 (sort
 * order of children).
 */
int matchExpressionComparator(const MatchExpression* lhs, const MatchExpression* rhs) {
    MatchExpression::MatchType lhsMatchType = lhs->matchType();
    MatchExpression::MatchType rhsMatchType = rhs->matchType();
    if (lhsMatchType != rhsMatchType) {
        return lhsMatchType < rhsMatchType ? -1 : 1;
    }

    StringData lhsPath = lhs->path();
    StringData rhsPath = rhs->path();
    int pathsCompare = lhsPath.compare(rhsPath);
    if (pathsCompare != 0) {
        return pathsCompare;
    }

    const size_t numChildren = std::min(lhs->numChildren(), rhs->numChildren());
    for (size_t childIdx = 0; childIdx < numChildren; ++childIdx) {
        int childCompare =
            matchExpressionComparator(lhs->getChild(childIdx), rhs->getChild(childIdx));
        if (childCompare != 0) {
            return childCompare;
        }
    }

    if (lhs->numChildren() != rhs->numChildren()) {
        return lhs->numChildren() < rhs->numChildren() ? -1 : 1;
    }

    // They're equal!
    return 0;
}

bool matchExpressionLessThan(const MatchExpression* lhs, const MatchExpression* rhs) {
    return matchExpressionComparator(lhs, rhs) < 0;
}

/**
 * Return true if the expression is trivially simple:
 * - has no children
 * - has one child without children
 * - rewritten simple $expr: contains one $expr and one simple simple expression without children.
 */
inline bool isTriviallySimple(const MatchExpression& expr) {
    switch (expr.numChildren()) {
        case 0:
            return true;
        case 1:
            return expr.getChild(0)->numChildren() == 0;
        case 2:
            // In the case of the rewritten simple $expr of the two nodes will be Internal
            // Expression Comparison node.
            return ComparisonMatchExpressionBase::isInternalExprComparison(
                       expr.getChild(0)->matchType()) ||
                ComparisonMatchExpressionBase::isInternalExprComparison(
                       expr.getChild(1)->matchType());
        default:
            return false;
    }
}

std::unique_ptr<MatchExpression> listOfOptimizer(std::unique_ptr<MatchExpression> expression) {
    auto& children = *static_cast<ListOfMatchExpression&>(*expression).getChildVector();

    // Recursively apply optimizations to child expressions.
    for (auto& childExpression : children)
        // The Boolean simplifier is disabled since we don't want to simplify sub-expressions,
        // but simplify the whole expression instead.
        childExpression = optimizeMatchExpression(std::move(childExpression),
                                                  /* enableSimplification */ false);

    // Associativity of AND and OR: an AND absorbs the children of any ANDs among its children
    // (and likewise for any OR with OR children).
    MatchExpression::MatchType matchType = expression->matchType();
    if (matchType == MatchExpression::AND || matchType == MatchExpression::OR) {
        auto absorbedExpressions = std::vector<std::unique_ptr<MatchExpression>>{};
        for (auto& childExpression : children) {
            if (childExpression->matchType() == matchType) {
                // Move this child out of the children array.
                auto childExpressionPtr = std::move(childExpression);
                childExpression = nullptr;  // Null out this child's entry in _expressions, so
                                            // that it will be deleted by the erase call below.

                // Move all of the grandchildren from the child expression to
                // absorbedExpressions.
                auto& grandChildren =
                    *static_cast<ListOfMatchExpression&>(*childExpressionPtr).getChildVector();
                std::move(grandChildren.begin(),
                          grandChildren.end(),
                          std::back_inserter(absorbedExpressions));
                grandChildren.clear();

                // Note that 'childExpressionPtr' will now be destroyed.
            }
        }

        // We replaced each destroyed child expression with nullptr. Now we remove those
        // nullptrs from the array.
        children.erase(std::remove(children.begin(), children.end(), nullptr), children.end());

        // Append the absorbed children to the end of the array.
        std::move(
            absorbedExpressions.begin(), absorbedExpressions.end(), std::back_inserter(children));
    }

    // Remove all children of AND that are $alwaysTrue and all children of OR and NOR that are
    // $alwaysFalse.
    if (matchType == MatchExpression::AND || matchType == MatchExpression::OR ||
        matchType == MatchExpression::NOR) {
        for (auto& childExpression : children)
            if ((childExpression->isTriviallyTrue() && matchType == MatchExpression::AND) ||
                (childExpression->isTriviallyFalse() && matchType == MatchExpression::OR) ||
                (childExpression->isTriviallyFalse() && matchType == MatchExpression::NOR))
                childExpression = nullptr;

        // We replaced each destroyed child expression with nullptr. Now we remove those
        // nullptrs from the vector.
        children.erase(std::remove(children.begin(), children.end(), nullptr), children.end());
    }

    // Check if the above optimizations eliminated all children. An OR with no children is
    // always false.
    if (children.empty() && matchType == MatchExpression::OR) {
        return std::make_unique<AlwaysFalseMatchExpression>();
    }
    // An AND with no children is always true and we need to return an
    // EmptyExpression. This ensures that the empty $and[] will be returned that serializes to
    // {} (SERVER-34759). A NOR with no children is always true. We treat an empty $nor[]
    // similarly.
    if (children.empty() &&
        (matchType == MatchExpression::AND || matchType == MatchExpression::NOR)) {
        return std::make_unique<AndMatchExpression>();
    }

    if (children.size() == 1) {
        if ((matchType == MatchExpression::AND || matchType == MatchExpression::OR ||
             matchType == MatchExpression::INTERNAL_SCHEMA_XOR)) {
            // Simplify AND/OR/XOR with exactly one operand to an expression consisting of just
            // that operand.
            auto simplifiedExpression = std::move(children.front());
            children.clear();
            return simplifiedExpression;
        } else if (matchType == MatchExpression::NOR && !children.front()->isTriviallyTrue()) {
            // All children of NOR that are $alwaysFalse are removed above. NOR containing an
            // expression that is $alwaysTrue is simplified to $alwaysFalse below. There is no
            // need to deal with these cases separately.
            // The else if statement above helps avoid invalid conversion from $nor+$alwaysTrue
            // to $not+$alwaysTrue.

            // Simplify NOR of exactly one operand to NOT of that operand in all other cases.
            auto simplifiedExpression =
                std::make_unique<NotMatchExpression>(std::move(children.front()));
            children.clear();
            return simplifiedExpression;
        }
    }

    if (matchType == MatchExpression::AND || matchType == MatchExpression::OR ||
        matchType == MatchExpression::NOR) {
        for (auto& childExpression : children) {
            // An AND containing an expression that always evaluates to false can be
            // optimized to a single $alwaysFalse expression.
            if (childExpression->isTriviallyFalse() && matchType == MatchExpression::AND) {
                return std::make_unique<AlwaysFalseMatchExpression>();
            }
            // Likewise, an OR containing an expression that always evaluates to true can be
            // optimized to a single $and[] expression that is trivially true and serializes to
            // {}. This "normalizes" the behaviour of true statements with $and and $or
            // (SERVER-34759).
            if (childExpression->isTriviallyTrue() && matchType == MatchExpression::OR) {
                return std::make_unique<AndMatchExpression>();
            }
            // A NOR containing an expression that always evaluates to true can be
            // optimized to a single $alwaysFalse expression.
            if (childExpression->isTriviallyTrue() && matchType == MatchExpression::NOR) {
                return std::make_unique<AlwaysFalseMatchExpression>();
            }
        }
    }

    // Rewrite an OR with EQ conditions on the same path as an IN-list. Example:
    // {$or: [{name: "Don"}, {name: "Alice"}]}
    // is rewritten as:
    // {name: {$in: ["Alice", "Don"]}}
    // Note that the simplification below groups all predicates eligible to be in an IN-list
    // together in one pass. For example, we will simplify the following:
    // {$or: [{name: "Don"}, {age: 30}, {age: 35}, {name: "Alice"}]}
    // to
    // {$or: [{name: {$in: ["Alice", "Don"]}}, {age: {$in: [30, 35]}}]}
    if (matchType == MatchExpression::OR && children.size() > 1) {
        // This groups the children which have equalities to scalars, arrays, and regexes
        // against the same path.
        stdx::unordered_map<std::string, std::vector<std::unique_ptr<MatchExpression>>>
            pathToExprsMap;

        // The children which we know cannot be part of this optimization.
        std::vector<std::unique_ptr<MatchExpression>> nonEligibleForIn;

        auto isRegEx = [](const BSONElement& elm) {
            return elm.type() == BSONType::regEx;
        };

        // Group the children together that have equality conditions or regular expressions on
        // the same paths. This step collects all of the information for each path (where there
        // is an appropriate predicate against it); we will filter out the paths that have only
        // one predicate against them after this.
        for (size_t i = 0; i < children.size(); i++) {
            auto& childExpression = children.at(i);
            if (childExpression->matchType() != MatchExpression::EQ &&
                childExpression->matchType() != MatchExpression::REGEX) {
                nonEligibleForIn.push_back(std::move(childExpression));
                continue;
            }

            if (childExpression->matchType() == MatchExpression::EQ) {
                auto eqExpression = static_cast<EqualityMatchExpression*>(childExpression.get());

                // Disjunctions of equalities use $eq comparison, which has different semantics
                // from $in for regular expressions. The regex under the equality is matched
                // literally as a string constant, while a regex inside $in is matched as a
                // regular expression. Furthermore, $lookup processing explicitly depends on
                // these different semantics.
                //
                // We should not attempt to rewrite an $eq:<regex> into $in because of these
                // different comparison semantics.
                if (isRegEx(eqExpression->getData())) {
                    nonEligibleForIn.push_back(std::move(childExpression));
                    continue;
                }
            }

            // Equality to parameterized constants should not participate in this rewrite
            // because parameter information is lost. For example, consider a predicate
            // {$or: [{a: 10}, {a: 20}]} where both the constants 10 and 20 are parameters; the
            // resulting expression {a: {$in: [10, 20]}} cannot be correctly rebound to a
            // predicate with different constants since we treat the $in operand as a single
            // parameter.
            if (childExpression->matchType() == MatchExpression::EQ) {
                auto eqExpression = static_cast<EqualityMatchExpression*>(childExpression.get());
                if (eqExpression->getInputParamId().has_value()) {
                    nonEligibleForIn.push_back(std::move(childExpression));
                    continue;
                }
            }

            auto key = std::string{childExpression->path()};
            if (!pathToExprsMap.contains(key)) {
                std::vector<std::unique_ptr<MatchExpression>> exprs;
                exprs.push_back(std::move(childExpression));
                pathToExprsMap.insert({key, std::move(exprs)});
            } else {
                auto& childrenIndexList = pathToExprsMap.find(key)->second;
                childrenIndexList.push_back(std::move(childExpression));
            }
        }

        // The number of predicates that will end up in a $in on their path.
        size_t numInEqualities = 0;

        // We only want to consider creating a $in expression for a field if there is more than
        // one predicate against it. Otherwise, that field is not eligible to have a $in.
        auto it = pathToExprsMap.begin();
        while (it != pathToExprsMap.end()) {
            tassert(8619400,
                    "Expecting at least one predicate against the path " + it->first,
                    it->second.size() > 0);
            if (it->second.size() == 1) {
                nonEligibleForIn.push_back(std::move(it->second.at(0)));
                pathToExprsMap.erase(it++);
            } else {
                numInEqualities += it->second.size();
                ++it;
            }
        }

        tassert(3401201,
                "All expressions must be classified as either eq-equiv or non-eq-equiv",
                numInEqualities + nonEligibleForIn.size() == children.size());

        // Create the $in expressions.
        if (!pathToExprsMap.empty()) {
            std::vector<std::unique_ptr<MatchExpression>> nonEquivOrChildren;
            nonEquivOrChildren.reserve(nonEligibleForIn.size());

            std::vector<std::unique_ptr<InMatchExpression>> ins;
            ins.reserve(pathToExprsMap.size());

            for (auto& pair : pathToExprsMap) {
                auto& path = pair.first;
                auto& exprs = pair.second;

                size_t numEqualitiesForPath = 0;

                // Because of the filtering we did earlier, we know that every path in the map
                // has more than one equality predicate against it.
                tassert(8619401,
                        "Expecting more than one one predicate against the path " + path,
                        exprs.size() > 1);

                auto inExpression = std::make_unique<InMatchExpression>(StringData(path));
                BSONArrayBuilder bab;

                // If at least one of the expressions that we will combine into the $in
                // expression is an equality, we will set the collator of the InMatchExpression
                // to be the collator of the first equality we encounter.
                const CollatorInterface* collator = nullptr;

                for (auto& expr : exprs) {
                    if (expr->matchType() == MatchExpression::EQ) {
                        std::unique_ptr<EqualityMatchExpression> eqExpressionPtr{
                            static_cast<EqualityMatchExpression*>(expr.release())};

                        if (!collator) {
                            collator = eqExpressionPtr->getCollator();
                        }

                        bab.append(eqExpressionPtr->getData());
                        ++numEqualitiesForPath;
                    } else if (expr->matchType() == MatchExpression::REGEX) {
                        std::unique_ptr<RegexMatchExpression> regexExpressionPtr{
                            static_cast<RegexMatchExpression*>(expr.release())};
                        // Reset the path because when we parse a $in expression which
                        // contains a regexp, we create a RegexMatchExpression with an
                        // empty path.
                        regexExpressionPtr->setPath({});
                        auto status = inExpression->addRegex(std::move(regexExpressionPtr));
                        tassert(3401203,  // TODO SERVER-53380 convert to tassertStatusOK.
                                "Conversion from OR to IN should always succeed",
                                status == Status::OK());
                    } else {
                        tasserted(8619402,
                                  "Expecting that the predicate against " + path +
                                      " is one of EQ or REGEX");
                    }
                }

                auto inEqualities = bab.obj();

                // Since the $in expression's elements haven't yet been initialized, we
                // have to manually count the equalities on the path (rather than calling
                // inExpression->getEqualities()).
                tassert(3401205,
                        "Incorrect number of in-equivalent expressions",
                        (numEqualitiesForPath + inExpression->getRegexes().size()) == exprs.size());

                // We may not necessarily have a collator for this path. This would occur if all
                // of the expressions in the original $or were regex ones and they were all put
                // into the $in expression. In this case, we will not set the $in expression's
                // collator.
                if (collator) {
                    inExpression->setCollator(collator);
                }

                auto status = inExpression->setEqualitiesArray(std::move(inEqualities));
                tassert(3401206,  // TODO SERVER-53380 convert to tassertStatusOK.
                        "Conversion from OR to IN should always succeed",
                        status == Status::OK());

                ins.push_back(std::move(inExpression));
            }

            // Once we know if there will be at least one $in expression we can generate, gather
            // all of the children which are not going to be part of a new $in.
            for (size_t i = 0; i < nonEligibleForIn.size(); i++) {
                nonEquivOrChildren.push_back(std::move(nonEligibleForIn.at(i)));
            }

            tassert(3401204,
                    "Incorrect number of non-equivalent expressions",
                    nonEquivOrChildren.size() == nonEligibleForIn.size());

            children.clear();

            // If every predicate in the original $or node ended up being transformed in to a
            // $in expression, we can drop the $or and just proceed with the $in.
            if (nonEquivOrChildren.size() == 0 && ins.size() == 1) {
                // The Boolean simplifier is disabled since we don't want to simplify
                // sub-expressions, but simplify the whole IN expression instead.
                // We recursively call optimize on this singular InMatchExpression to take care
                // of the case where we rewrite {$or: [{a: 1}, {a: 1}]} and after deduplication,
                // there is only one element in the $in list. The
                // InMatchExpression::getOptimizer() simplifies this so that there is no $in,
                // and the whole thing becomes {a: 1}.
                return optimizeMatchExpression(std::move(ins.at(0)), false);
            }

            auto parentOrExpr = std::make_unique<OrMatchExpression>();
            auto&& childVec = *parentOrExpr->getChildVector();
            childVec.reserve(ins.size() + nonEquivOrChildren.size());

            // Move any newly constructed InMatchExpressions to be children of the new $or node.
            std::move(std::make_move_iterator(ins.begin()),
                      std::make_move_iterator(ins.end()),
                      std::back_inserter(childVec));

            // Move any non-equivalent children of the original $or so that they become children
            // of the newly constructed $or node.
            std::move(std::make_move_iterator(nonEquivOrChildren.begin()),
                      std::make_move_iterator(nonEquivOrChildren.end()),
                      std::back_inserter(childVec));

            return parentOrExpr;
        } else {
            // If the map is empty, there are no $in expressions to create. We should put all
            // the children back that we deemed ineligible.
            children.clear();
            for (size_t i = 0; i < nonEligibleForIn.size(); i++) {
                children.push_back(std::move(nonEligibleForIn.at(i)));
            }
        }
    }
    return expression;
}

std::unique_ptr<MatchExpression> notOptimizer(std::unique_ptr<MatchExpression> expression) {
    auto& notExpression = static_cast<NotMatchExpression&>(*expression);
    std::unique_ptr<MatchExpression> child(notExpression.releaseChild());
    // The Boolean simplifier is disabled since we don't want to simplify sub-expressions, but
    // simplify the whole expression instead.
    notExpression.resetChild(0,
                             optimizeMatchExpression(std::move(child),
                                                     /* enableSimplification */ false)
                                 .release());

    return expression;
}

template <typename T, size_t nargs>
std::unique_ptr<MatchExpression> fixedArityOptimizer(std::unique_ptr<MatchExpression> expression) {
    auto& fixedArityExpression = static_cast<FixedArityMatchExpression<T, nargs>&>(*expression);

    for (size_t childNum = 0; childNum < fixedArityExpression.numChildren(); ++childNum) {
        std::unique_ptr<MatchExpression> subExpression(fixedArityExpression.releaseChild(childNum));

        // The Boolean simplifier is disabled since we don't want to simplify sub-expressions, but
        // simplify the whole expression instead.
        fixedArityExpression.resetChild(childNum,
                                        optimizeMatchExpression(std::move(subExpression),
                                                                /* enableSimplification */ false)
                                            .release());
    }

    return expression;
}
constexpr auto condMatchOptimizer = fixedArityOptimizer<InternalSchemaCondMatchExpression, 3>;

std::unique_ptr<MatchExpression> elemMatchObjectOptimizer(
    std::unique_ptr<MatchExpression> expression) {
    auto& elemExpression = static_cast<ElemMatchObjectMatchExpression&>(*expression);

    // The Boolean simplifier is disabled since we don't want to simplify sub-expressions, but
    // simplify the whole expression instead.
    elemExpression.resetChild(0,
                              optimizeMatchExpression(elemExpression.releaseChild(),
                                                      /* enableSimplification */ false)
                                  .release());

    if (elemExpression.getChild(0)->isTriviallyFalse()) {
        return std::make_unique<AlwaysFalseMatchExpression>();
    }

    return expression;
}

std::unique_ptr<MatchExpression> elemMatchValueOptimizer(
    std::unique_ptr<MatchExpression> expression) {
    auto& subs = *static_cast<ElemMatchValueMatchExpression&>(*expression).getChildVector();

    for (auto& subExpression : subs) {
        // The Boolean simplifier is disabled since we don't want to simplify sub-expressions,
        // but simplify the whole expression instead.
        subExpression = optimizeMatchExpression(std::move(subExpression),
                                                /* enableSimplification */ false);
        if (subExpression->isTriviallyFalse()) {
            return std::make_unique<AlwaysFalseMatchExpression>();
        }
    }

    return expression;
}

// Return nullptr on failure.
std::unique_ptr<MatchExpression> attemptToRewriteEqHash(ExprMatchExpression& expr) {
    auto childExpr = expr.getExpression();

    // Looking for:
    //                     $eq
    //    $toHashedIndexKey   {$const: NumberLong(?)}
    //           "$a"
    //
    // Where "a" can be any field path and ? can be any number.
    if (auto eq = dynamic_cast<ExpressionCompare*>(childExpr.get());
        eq && eq->getOp() == ExpressionCompare::CmpOp::EQ) {
        const auto& children = eq->getChildren();
        tassert(7281406, "should have 2 $eq children", children.size() == 2ul);

        auto eqFirst = children[0].get();
        auto eqSecond = children[1].get();
        if (auto hashingExpr = dynamic_cast<ExpressionToHashedIndexKey*>(eqFirst)) {
            // Matched $toHashedIndexKey - keep going.
            tassert(7281407,
                    "should have 1 $toHashedIndexKey child",
                    hashingExpr->getChildren().size() == 1ul);
            auto hashChild = hashingExpr->getChildren()[0].get();

            if (auto fieldPath = dynamic_cast<ExpressionFieldPath*>(hashChild);
                fieldPath && !fieldPath->isVariableReference() && !fieldPath->isROOT()) {
                auto path = fieldPath->getFieldPathWithoutCurrentPrefix();

                // Matched "$a" in the example above! Now look for the constant long:
                if (auto constant = dynamic_cast<ExpressionConstant*>(eqSecond);
                    constant && constant->getValue().getType() == BSONType::numberLong) {
                    long long hashTarget = constant->getValue().getLong();
                    return std::make_unique<InternalEqHashedKey>(path.fullPath(), hashTarget);
                }
            }
        }
    }
    return nullptr;
}

std::unique_ptr<MatchExpression> exprOptimizer(std::unique_ptr<MatchExpression> expression) {
    auto& exprMatchExpr = static_cast<ExprMatchExpression&>(*expression);

    //  $expr expressions can't take advantage of indexes. We attempt to rewrite the expressions
    //  as a conjunction of internal match expressions, so the query planner can use the
    //  internal match expressions to potentially generate an index scan.
    // Exiting early prevents additional calls to optimize from performing additional rewrites
    // and adding duplicate MatchExpression sub-trees to the tree.
    if (exprMatchExpr.getRewriteResult()) {
        return expression;
    }

    exprMatchExpr.getExpressionRef() = exprMatchExpr.getExpressionRef()->optimize();
    if (auto successfulEqHashRewrite = attemptToRewriteEqHash(exprMatchExpr)) {
        return successfulEqHashRewrite;
    }

    exprMatchExpr.setRewriteResult(RewriteExpr::rewrite(
        exprMatchExpr.getExpression(), exprMatchExpr.getExpressionContext()->getCollator()));

    if (exprMatchExpr.getRewriteResult()->matchExpression()) {
        // If 'expression' can be rewritten to a MatchExpression, we will return a $and node
        // with both the original ExprMatchExpression and the MatchExpression rewrite as
        // children. The rewritten expression might not be equivalent to the original one so we
        // still have to keep the latter for correctness.
        auto andMatch = std::make_unique<AndMatchExpression>();
        andMatch->add(exprMatchExpr.getRewriteResult()->releaseMatchExpression());
        andMatch->add(std::move(expression));
        // Re-optimize the new AND in order to make sure that any AND children are absorbed.
        // The Boolean simplifier is disabled since we don't want to simplify sub-expressions,
        // but simplify the whole expression instead.
        expression = optimizeMatchExpression(std::move(andMatch), /* enableSimplification */ false);
    }

    // Replace trivially true expression with an empty AND since the planner doesn't always
    // check for 'isTriviallyTrue()'.
    if (expression->isTriviallyTrue()) {
        expression = std::make_unique<AndMatchExpression>();
    }

    if (expression->isTriviallyFalse()) {
        expression = std::make_unique<AlwaysFalseMatchExpression>();
    }

    return expression;
}

std::unique_ptr<MatchExpression> inOptimizer(std::unique_ptr<MatchExpression> expression) {
    // NOTE: We do not recursively call optimize() on the RegexMatchExpression children in the
    // _regexes list. We assume that optimize() on a RegexMatchExpression is a no-op.
    auto& ime = static_cast<InMatchExpression&>(*expression);
    auto& regexes = ime.getRegexes();
    auto collator = ime.getCollator();

    if (regexes.size() == 1 && ime.equalitiesIsEmpty()) {
        // Simplify IN of exactly one regex to be a regex match.
        auto& childRe = regexes.front();
        invariant(!childRe->getTag());

        auto simplifiedExpression = std::make_unique<RegexMatchExpression>(
            expression->path(), childRe->getString(), childRe->getFlags());
        if (expression->getTag()) {
            simplifiedExpression->setTag(expression->getTag()->clone());
        }
        return simplifiedExpression;
    } else if (ime.equalitiesHasSingleElement() && regexes.empty()) {
        // Simplify IN of exactly one equality to be an EqualityMatchExpression.
        BSONObj obj(BSON(expression->path() << *(ime.getInListDataPtr()->getElements().begin())));
        auto simplifiedExpression =
            std::make_unique<EqualityMatchExpression>(expression->path(), obj.firstElement());
        simplifiedExpression->setBackingBSON(obj);

        simplifiedExpression->setCollator(collator);
        if (expression->getTag()) {
            simplifiedExpression->setTag(expression->getTag()->clone());
        }

        return simplifiedExpression;
    } else if (regexes.empty() && ime.equalitiesIsEmpty()) {
        // Empty IN is always false
        return std::make_unique<AlwaysFalseMatchExpression>();
    }

    return expression;
}

std::unique_ptr<MatchExpression> internalSchemaAllElemMatchFromIndexOptimizer(
    std::unique_ptr<MatchExpression> expression) {
    auto& allElemMatchFromIndexExpr =
        static_cast<InternalSchemaAllElemMatchFromIndexMatchExpression&>(*expression);
    std::unique_ptr<MatchExpression> filter(
        allElemMatchFromIndexExpr.getExpression()->releaseFilter());
    // The Boolean simplifier is disabled since we don't want to simplify sub-expressions, but
    // simplify the whole expression instead.
    allElemMatchFromIndexExpr.getExpression()->resetFilter(
        optimizeMatchExpression(std::move(filter), /* enableSimplification */ false).release());
    return expression;
}

std::unique_ptr<MatchExpression> internalSchemaAllowedPropertiesOptimizer(
    std::unique_ptr<MatchExpression> expression) {
    auto& allowedPropertiesExpr =
        static_cast<InternalSchemaAllowedPropertiesMatchExpression&>(*expression);

    for (size_t childNum = 0; childNum < allowedPropertiesExpr.numChildren(); ++childNum) {
        std::unique_ptr<MatchExpression> child(allowedPropertiesExpr.releaseChild(childNum));

        // The Boolean simplifier is disabled since we don't want to simplify sub-expressions, but
        // simplify the whole expression instead.
        allowedPropertiesExpr.resetChild(childNum,
                                         optimizeMatchExpression(std::move(child),
                                                                 /* enableSimplification */ false)
                                             .release());
    }

    return expression;
}

std::unique_ptr<MatchExpression> internalSchemaMatchArrayIndexOptimizer(
    std::unique_ptr<MatchExpression> expression) {
    auto& matchArrayIndexExpr =
        static_cast<InternalSchemaMatchArrayIndexMatchExpression&>(*expression);
    std::unique_ptr<MatchExpression> filter(matchArrayIndexExpr.getExpression()->releaseFilter());
    // The Boolean simplifier is disabled since we don't want to simplify sub-expressions, but
    // simplify the whole expression instead.
    matchArrayIndexExpr.getExpression()->resetFilter(
        optimizeMatchExpression(std::move(filter), /* enableSimplification */ false).release());
    return expression;
}

std::unique_ptr<MatchExpression> internalSchemaObjectOptimizer(
    std::unique_ptr<MatchExpression> expression) {
    auto& objectMatchExpression = static_cast<InternalSchemaObjectMatchExpression&>(*expression);
    // The Boolean simplifier does not support schema expressions for we haven't figured out how
    // to simplify them and whether we want them to be simplified or not.
    std::unique_ptr<MatchExpression> child(objectMatchExpression.releaseChild());
    objectMatchExpression.resetChild(0,
                                     optimizeMatchExpression(std::move(child),
                                                             /* enableSimplification */ false)
                                         .release());
    return expression;
}

REGISTER_MATCH_EXPRESSION_OPTIMIZER(AND, MatchExpression::AND, listOfOptimizer)
REGISTER_MATCH_EXPRESSION_OPTIMIZER(OR, MatchExpression::OR, listOfOptimizer)
REGISTER_MATCH_EXPRESSION_OPTIMIZER(NOR, MatchExpression::NOR, listOfOptimizer)
REGISTER_MATCH_EXPRESSION_OPTIMIZER(NOT, MatchExpression::NOT, notOptimizer)
REGISTER_MATCH_EXPRESSION_OPTIMIZER(INTERNAL_SCHEMA_COND,
                                    MatchExpression::INTERNAL_SCHEMA_COND,
                                    condMatchOptimizer)
REGISTER_MATCH_EXPRESSION_OPTIMIZER(ELEM_MATCH_OBJECT,
                                    MatchExpression::ELEM_MATCH_OBJECT,
                                    elemMatchObjectOptimizer)
REGISTER_MATCH_EXPRESSION_OPTIMIZER(ELEM_MATCH_VALUE,
                                    MatchExpression::ELEM_MATCH_VALUE,
                                    elemMatchValueOptimizer)
REGISTER_MATCH_EXPRESSION_OPTIMIZER(EXPRESSION, MatchExpression::EXPRESSION, exprOptimizer)
REGISTER_MATCH_EXPRESSION_OPTIMIZER(MATCH_IN, MatchExpression::MATCH_IN, inOptimizer)
REGISTER_MATCH_EXPRESSION_OPTIMIZER(INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX,
                                    MatchExpression::INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX,
                                    internalSchemaAllElemMatchFromIndexOptimizer)
REGISTER_MATCH_EXPRESSION_OPTIMIZER(INTERNAL_SCHEMA_ALLOWED_PROPERTIES,
                                    MatchExpression::INTERNAL_SCHEMA_ALLOWED_PROPERTIES,
                                    internalSchemaAllowedPropertiesOptimizer)
REGISTER_MATCH_EXPRESSION_OPTIMIZER(INTERNAL_SCHEMA_MATCH_ARRAY_INDEX,
                                    MatchExpression::INTERNAL_SCHEMA_MATCH_ARRAY_INDEX,
                                    internalSchemaMatchArrayIndexOptimizer)
REGISTER_MATCH_EXPRESSION_OPTIMIZER(INTERNAL_SCHEMA_OBJECT_MATCH,
                                    MatchExpression::INTERNAL_SCHEMA_OBJECT_MATCH,
                                    internalSchemaObjectOptimizer)
REGISTER_MATCH_EXPRESSION_OPTIMIZER(INTERNAL_SCHEMA_XOR,
                                    MatchExpression::INTERNAL_SCHEMA_XOR,
                                    listOfOptimizer)

}  // namespace

std::unique_ptr<MatchExpression> optimizeMatchExpression(
    std::unique_ptr<MatchExpression> expression, bool enableSimplification) {
    tassert(10806403, "Optimized expression cannot be nullptr", expression != nullptr);

    // If the disableMatchExpressionOptimization failpoint is enabled, optimizations are skipped
    // and the expression is left unmodified.
    if (MONGO_unlikely(disableMatchExpressionOptimization.shouldFail())) {
        return expression;
    }

    try {
        auto optimizer = matchExpressionOptimizersMap.find(expression->matchType());
        auto optimizedExpr = optimizer != matchExpressionOptimizersMap.end()
            ? optimizer->second(std::move(expression))
            : std::move(expression);
        if (enableSimplification && internalQueryEnableBooleanExpressionsSimplifier.load()) {
            if (isTriviallySimple(*optimizedExpr)) {
                expressionSimplifierMetrics.trivialCount.incrementRelaxed();
                return optimizedExpr;
            }
            ExpressionSimplifierSettings settings{
                static_cast<size_t>(internalQueryMaximumNumberOfUniquePredicatesToSimplify.load()),
                static_cast<size_t>(internalQueryMaximumNumberOfMintermsInSimplifier.load()),
                internalQueryMaxSizeFactorToSimplify.load(),
                internalQueryDoNotOpenContainedOrsInSimplifier.load(),
                /*applyQuineMcCluskey*/ true};
            auto simplifiedExpr = simplifyMatchExpression(optimizedExpr.get(), settings);
            if (simplifiedExpr) {
                return std::move(*simplifiedExpr);
            }
        }
        return optimizedExpr;
    } catch (DBException& ex) {
        ex.addContext("Failed to optimize expression");
        throw;
    }
}

void sortMatchExpressionTree(MatchExpression* tree) {
    tassert(10806404, "Sorting matching expression cannot be nullptr", tree != nullptr);

    for (size_t i = 0; i < tree->numChildren(); ++i) {
        sortMatchExpressionTree(tree->getChild(i));
    }
    if (auto&& children = tree->getChildVector()) {
        std::stable_sort(children->begin(), children->end(), [](auto&& lhs, auto&& rhs) {
            return matchExpressionLessThan(lhs.get(), rhs.get());
        });
    }
}

std::unique_ptr<MatchExpression> normalizeMatchExpression(std::unique_ptr<MatchExpression> tree,
                                                          bool enableSimplification) {
    tassert(10806405, "Normalized expression cannot be nullptr", tree != nullptr);

    tree = optimizeMatchExpression(std::move(tree), enableSimplification);
    sortMatchExpressionTree(tree.get());
    return tree;
}

}  // namespace mongo
