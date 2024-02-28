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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <iterator>
#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_path.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/query/collation/collator_interface.h"

namespace mongo {
namespace {

PathMatchExpression* getEligiblePathMatchForNotSerialization(MatchExpression* expr) {
    // Returns a pointer to a PathMatchExpression if 'expr' is such a pointer, otherwise returns
    // nullptr.
    //
    // One exception: while TextMatchExpressionBase derives from PathMatchExpression, text match
    // expressions cannot be serialized in the same manner as other PathMatchExpression derivatives.
    // This is because the path for a TextMatchExpression is embedded within the $text object,
    // whereas for other PathMatchExpressions it is on the left-hand-side, for example {x: {$eq:
    // 1}}.
    //
    // Rather than the following dynamic_cast, we'll do a more performant, but also more verbose
    // check.
    // dynamic_cast<PathMatchExpression*>(expr) && !dynamic_cast<TextMatchExpressionBase*>(expr)
    //
    // This version below is less obviously exhaustive, but because this is just a legibility
    // optimization, and this function also gets called on the query shape stats recording hot path,
    // we think it is worth it.
    switch (expr->matchType()) {
        // leaf types
        case MatchExpression::EQ:
        case MatchExpression::LTE:
        case MatchExpression::LT:
        case MatchExpression::GT:
        case MatchExpression::GTE:
        case MatchExpression::REGEX:
        case MatchExpression::MOD:
        case MatchExpression::EXISTS:
        case MatchExpression::MATCH_IN:
        case MatchExpression::BITS_ALL_SET:
        case MatchExpression::BITS_ALL_CLEAR:
        case MatchExpression::BITS_ANY_SET:
        case MatchExpression::BITS_ANY_CLEAR:
        // array types
        case MatchExpression::ELEM_MATCH_OBJECT:
        case MatchExpression::ELEM_MATCH_VALUE:
        case MatchExpression::SIZE:
        // special types
        case MatchExpression::TYPE_OPERATOR:
        case MatchExpression::GEO:
        case MatchExpression::GEO_NEAR:
        // Internal subclasses of PathMatchExpression:
        case MatchExpression::INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX:
        case MatchExpression::INTERNAL_SCHEMA_BIN_DATA_ENCRYPTED_TYPE:
        case MatchExpression::INTERNAL_SCHEMA_BIN_DATA_FLE2_ENCRYPTED_TYPE:
        case MatchExpression::INTERNAL_SCHEMA_BIN_DATA_SUBTYPE:
        case MatchExpression::INTERNAL_SCHEMA_MATCH_ARRAY_INDEX:
        case MatchExpression::INTERNAL_SCHEMA_MAX_ITEMS:
        case MatchExpression::INTERNAL_SCHEMA_MAX_LENGTH:
        case MatchExpression::INTERNAL_SCHEMA_MAX_PROPERTIES:
        case MatchExpression::INTERNAL_SCHEMA_MIN_ITEMS:
        case MatchExpression::INTERNAL_SCHEMA_MIN_LENGTH:
        case MatchExpression::INTERNAL_SCHEMA_TYPE:
        case MatchExpression::INTERNAL_SCHEMA_UNIQUE_ITEMS:
            return static_cast<PathMatchExpression*>(expr);
        // purposefully skip TEXT:
        case MatchExpression::TEXT:
        // Any other type is not considered a PathMatchExpression.
        case MatchExpression::AND:
        case MatchExpression::OR:
        case MatchExpression::NOT:
        case MatchExpression::NOR:
        case MatchExpression::WHERE:
        case MatchExpression::EXPRESSION:
        case MatchExpression::ALWAYS_FALSE:
        case MatchExpression::ALWAYS_TRUE:
        case MatchExpression::INTERNAL_2D_POINT_IN_ANNULUS:
        case MatchExpression::INTERNAL_BUCKET_GEO_WITHIN:
        case MatchExpression::INTERNAL_EXPR_EQ:
        case MatchExpression::INTERNAL_EXPR_GT:
        case MatchExpression::INTERNAL_EXPR_GTE:
        case MatchExpression::INTERNAL_EXPR_LT:
        case MatchExpression::INTERNAL_EXPR_LTE:
        case MatchExpression::INTERNAL_EQ_HASHED_KEY:
        case MatchExpression::INTERNAL_SCHEMA_ALLOWED_PROPERTIES:
        case MatchExpression::INTERNAL_SCHEMA_COND:
        case MatchExpression::INTERNAL_SCHEMA_EQ:
        case MatchExpression::INTERNAL_SCHEMA_FMOD:
        case MatchExpression::INTERNAL_SCHEMA_MIN_PROPERTIES:
        case MatchExpression::INTERNAL_SCHEMA_OBJECT_MATCH:
        case MatchExpression::INTERNAL_SCHEMA_ROOT_DOC_EQ:
        case MatchExpression::INTERNAL_SCHEMA_XOR:
            return nullptr;
        default:
            MONGO_UNREACHABLE_TASSERT(7800300);
    }
};
}  // namespace

void ListOfMatchExpression::_debugList(StringBuilder& debug, int indentationLevel) const {
    for (unsigned i = 0; i < _expressions.size(); i++)
        _expressions[i]->debugString(debug, indentationLevel + 1);
}

void ListOfMatchExpression::_listToBSON(BSONArrayBuilder* out,
                                        const SerializationOptions& opts,
                                        bool includePath) const {
    for (unsigned i = 0; i < _expressions.size(); i++) {
        BSONObjBuilder childBob(out->subobjStart());
        _expressions[i]->serialize(&childBob, opts, includePath);
    }
    out->doneFast();
}

MatchExpression::ExpressionOptimizerFunc ListOfMatchExpression::getOptimizer() const {
    return [](std::unique_ptr<MatchExpression> expression) -> std::unique_ptr<MatchExpression> {
        auto& children = static_cast<ListOfMatchExpression&>(*expression)._expressions;

        // Recursively apply optimizations to child expressions.
        for (auto& childExpression : children)
            // The Boolean simplifier is disabled since we don't want to simplify sub-expressions,
            // but simplify the whole expression instead.
            childExpression = MatchExpression::optimize(std::move(childExpression),
                                                        /* enableSimplification */ false);

        // Associativity of AND and OR: an AND absorbs the children of any ANDs among its children
        // (and likewise for any OR with OR children).
        MatchType matchType = expression->matchType();
        if (matchType == AND || matchType == OR) {
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
                        static_cast<ListOfMatchExpression&>(*childExpressionPtr)._expressions;
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
            std::move(absorbedExpressions.begin(),
                      absorbedExpressions.end(),
                      std::back_inserter(children));
        }

        // Remove all children of AND that are $alwaysTrue and all children of OR and NOR that are
        // $alwaysFalse.
        if (matchType == AND || matchType == OR || matchType == NOR) {
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
            if ((matchType == AND || matchType == OR || matchType == INTERNAL_SCHEMA_XOR)) {
                // Simplify AND/OR/XOR with exactly one operand to an expression consisting of just
                // that operand.
                auto simplifiedExpression = std::move(children.front());
                children.clear();
                return simplifiedExpression;
            } else if (matchType == NOR) {
                // Simplify NOR of exactly one operand to NOT of that operand.
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
                return elm.type() == BSONType::RegEx;
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
                    auto eqExpression =
                        static_cast<EqualityMatchExpression*>(childExpression.get());

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
                    auto eqExpression =
                        static_cast<EqualityMatchExpression*>(childExpression.get());
                    if (eqExpression->getInputParamId().has_value()) {
                        nonEligibleForIn.push_back(std::move(childExpression));
                        continue;
                    }
                }

                auto key = childExpression->path().toString();
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

                    // Since the $in expression's InListData elements haven't yet been initialized,
                    // we have to manually count the equalities on the path (rather than calling
                    // inExpression->getEqualities()).
                    tassert(3401205,
                            "Incorrect number of in-equivalent expressions",
                            (numEqualitiesForPath + inExpression->getRegexes().size()) ==
                                exprs.size());

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
                    return MatchExpression::optimize(std::move(ins.at(0)), false);
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
    };
}

bool ListOfMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const ListOfMatchExpression* realOther = static_cast<const ListOfMatchExpression*>(other);

    if (_expressions.size() != realOther->_expressions.size())
        return false;

    // TOOD: order doesn't matter
    for (unsigned i = 0; i < _expressions.size(); i++)
        if (!_expressions[i]->equivalent(realOther->_expressions[i].get()))
            return false;

    return true;
}

// -----

bool AndMatchExpression::matches(const MatchableDocument* doc, MatchDetails* details) const {
    for (size_t i = 0; i < numChildren(); i++) {
        if (!getChild(i)->matches(doc, details)) {
            if (details)
                details->resetOutput();
            return false;
        }
    }
    return true;
}

bool AndMatchExpression::matchesSingleElement(const BSONElement& e, MatchDetails* details) const {
    for (size_t i = 0; i < numChildren(); i++) {
        if (!getChild(i)->matchesSingleElement(e, details)) {
            return false;
        }
    }
    return true;
}


void AndMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << "$and";
    _debugStringAttachTagInfo(&debug);
    _debugList(debug, indentationLevel);
}

void AndMatchExpression::serialize(BSONObjBuilder* out,
                                   const SerializationOptions& opts,
                                   bool includePath) const {
    if (!numChildren()) {
        // It is possible for an AndMatchExpression to have no children, resulting in the serialized
        // expression {$and: []}, which is not a valid query object.
        return;
    }

    BSONArrayBuilder arrBob(out->subarrayStart("$and"));
    _listToBSON(&arrBob, opts, includePath);
    arrBob.doneFast();
}

bool AndMatchExpression::isTriviallyTrue() const {
    return numChildren() == 0;
}

// -----

bool OrMatchExpression::matches(const MatchableDocument* doc, MatchDetails* details) const {
    for (size_t i = 0; i < numChildren(); i++) {
        if (getChild(i)->matches(doc, nullptr)) {
            return true;
        }
    }
    return false;
}

bool OrMatchExpression::matchesSingleElement(const BSONElement& e, MatchDetails* details) const {
    MONGO_UNREACHABLE_TASSERT(5429901);
}


void OrMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << "$or";
    _debugStringAttachTagInfo(&debug);
    _debugList(debug, indentationLevel);
}

void OrMatchExpression::serialize(BSONObjBuilder* out,
                                  const SerializationOptions& opts,
                                  bool includePath) const {
    if (!numChildren()) {
        // It is possible for an OrMatchExpression to have no children, resulting in the serialized
        // expression {$or: []}, which is not a valid query object. An empty $or is logically
        // equivalent to {$alwaysFalse: 1}.
        out->append(AlwaysFalseMatchExpression::kName, 1);
        return;
    }
    BSONArrayBuilder arrBob(out->subarrayStart("$or"));
    _listToBSON(&arrBob, opts, includePath);
}

bool OrMatchExpression::isTriviallyFalse() const {
    return numChildren() == 0;
}

// ----

bool NorMatchExpression::matches(const MatchableDocument* doc, MatchDetails* details) const {
    for (size_t i = 0; i < numChildren(); i++) {
        if (getChild(i)->matches(doc, nullptr)) {
            return false;
        }
    }
    return true;
}

bool NorMatchExpression::matchesSingleElement(const BSONElement& e, MatchDetails* details) const {
    for (size_t i = 0; i < numChildren(); i++) {
        if (getChild(i)->matchesSingleElement(e, details)) {
            return false;
        }
    }
    return true;
}

void NorMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << "$nor";
    _debugStringAttachTagInfo(&debug);
    _debugList(debug, indentationLevel);
}

void NorMatchExpression::serialize(BSONObjBuilder* out,
                                   const SerializationOptions& opts,
                                   bool includePath) const {
    BSONArrayBuilder arrBob(out->subarrayStart("$nor"));
    _listToBSON(&arrBob, opts, includePath);
}

// -------

void NotMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << "$not";
    _debugStringAttachTagInfo(&debug);
    _exp->debugString(debug, indentationLevel + 1);
}

void NotMatchExpression::serializeNotExpressionToNor(MatchExpression* exp,
                                                     BSONObjBuilder* out,
                                                     const SerializationOptions& opts,
                                                     bool includePath) {
    BSONObjBuilder childBob;
    exp->serialize(&childBob, opts, includePath);
    BSONObj tempObj = childBob.obj();

    BSONArrayBuilder tBob(out->subarrayStart("$nor"));
    tBob.append(tempObj);
    tBob.doneFast();
}

void NotMatchExpression::serialize(BSONObjBuilder* out,
                                   const SerializationOptions& opts,
                                   bool includePath) const {
    if (_exp->matchType() == MatchType::AND && _exp->numChildren() == 0) {
        opts.appendLiteral(out, "$alwaysFalse", 1);
        return;
    }

    if (!includePath) {
        BSONObjBuilder notBob(out->subobjStart("$not"));
        // Our parser does not accept a $and directly within a $not, instead expecting the direct
        // notation like {x: {$not: {$gt: 5, $lt: 0}}}. We represent such an expression with an AND
        // internally, so we un-nest it here to be able to re-parse it.
        if (_exp->matchType() == MatchType::AND) {
            for (size_t x = 0; x < _exp->numChildren(); ++x) {
                _exp->getChild(x)->serialize(&notBob, opts, includePath);
            }
        } else {
            _exp->serialize(&notBob, opts, includePath);
        }
        return;
    }

    auto expressionToNegate = _exp.get();
    if (_exp->matchType() == MatchType::AND && _exp->numChildren() == 1) {
        expressionToNegate = _exp->getChild(0);
    }

    // It is generally easier to be correct if we just always serialize to a $nor, since this will
    // delegate the path serialization to lower in the tree where we have the information on-hand.
    // However, for legibility we preserve a $not with a single path-accepting child as a $not.
    if (auto pathMatch = getEligiblePathMatchForNotSerialization(expressionToNegate)) {
        auto append = [&](StringData path) {
            BSONObjBuilder pathBob(out->subobjStart(path));
            pathBob.append("$not", pathMatch->getSerializedRightHandSide(opts));
        };
        append(opts.serializeFieldPathFromString(pathMatch->path()));
        return;
    }
    return serializeNotExpressionToNor(expressionToNegate, out, opts);
}

bool NotMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    return _exp->equivalent(other->getChild(0));
}


MatchExpression::ExpressionOptimizerFunc NotMatchExpression::getOptimizer() const {
    return [](std::unique_ptr<MatchExpression> expression) {
        auto& notExpression = static_cast<NotMatchExpression&>(*expression);
        // The Boolean simplifier is disabled since we don't want to simplify sub-expressions, but
        // simplify the whole expression instead.
        notExpression._exp = MatchExpression::optimize(std::move(notExpression._exp),
                                                       /* enableSimplification */ false);

        return expression;
    };
}
}  // namespace mongo
