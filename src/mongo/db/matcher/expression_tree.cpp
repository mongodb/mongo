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

#include <algorithm>
#include <iterator>

#include "mongo/db/matcher/expression_tree.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_path.h"
#include "mongo/db/matcher/expression_text_base.h"

namespace mongo {

void ListOfMatchExpression::_debugList(StringBuilder& debug, int indentationLevel) const {
    for (unsigned i = 0; i < _expressions.size(); i++)
        _expressions[i]->debugString(debug, indentationLevel + 1);
}

void ListOfMatchExpression::_listToBSON(BSONArrayBuilder* out, SerializationOptions opts) const {
    for (unsigned i = 0; i < _expressions.size(); i++) {
        BSONObjBuilder childBob(out->subobjStart());
        _expressions[i]->serialize(&childBob, opts);
    }
    out->doneFast();
}

MatchExpression::ExpressionOptimizerFunc ListOfMatchExpression::getOptimizer() const {
    return [](std::unique_ptr<MatchExpression> expression) -> std::unique_ptr<MatchExpression> {
        auto& children = static_cast<ListOfMatchExpression&>(*expression)._expressions;

        // Recursively apply optimizations to child expressions.
        for (auto& childExpression : children)
            childExpression = MatchExpression::optimize(std::move(childExpression));

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

        // Remove all children of AND that are $alwaysTrue and all children of OR that are
        // $alwaysFalse.
        if (matchType == AND || matchType == OR) {
            for (auto& childExpression : children)
                if ((childExpression->isTriviallyTrue() && matchType == MatchExpression::AND) ||
                    (childExpression->isTriviallyFalse() && matchType == MatchExpression::OR))
                    childExpression = nullptr;

            // We replaced each destroyed child expression with nullptr. Now we remove those
            // nullptrs from the vector.
            children.erase(std::remove(children.begin(), children.end(), nullptr), children.end());
        }

        // Check if the above optimizations eliminated all children. An OR with no children is
        // always false.
        // TODO SERVER-34759 It is correct to replace this empty AND with an $alwaysTrue, but we
        // need to make enhancements to the planner to make it understand an $alwaysTrue and an
        // empty AND as the same thing. The planner can create inferior plans for $alwaysTrue which
        // it would not produce for an AND with no children.
        if (children.empty() && matchType == MatchExpression::OR) {
            return std::make_unique<AlwaysFalseMatchExpression>();
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

        if (matchType == MatchExpression::AND || matchType == MatchExpression::OR) {
            for (auto& childExpression : children) {
                // An AND containing an expression that always evaluates to false can be
                // optimized to a single $alwaysFalse expression.
                if (childExpression->isTriviallyFalse() && matchType == MatchExpression::AND) {
                    return std::make_unique<AlwaysFalseMatchExpression>();
                }

                // Likewise, an OR containing an expression that always evaluates to true can be
                // optimized to a single $alwaysTrue expression.
                if (childExpression->isTriviallyTrue() && matchType == MatchExpression::OR) {
                    return std::make_unique<AlwaysTrueMatchExpression>();
                }
            }
        }

        // Rewrite an OR with EQ conditions on the same path as an IN-list. Example:
        // {$or: [{name: "Don"}, {name: "Alice"}]}
        // is rewritten as:
        // {name: {$in: ["Alice", "Don"]}}
        if (matchType == MatchExpression::OR && children.size() > 1) {
            size_t countEquivEqPaths = 0;
            size_t countNonEquivExpr = 0;
            boost::optional<std::string> childPath;
            // The collation of the first equality. All other equalities must have the same
            // collation in order to transform them into a single $in since the $in can have only
            // one collation. Notice that regex ignore collations.
            const CollatorInterface* eqCollator = nullptr;

            auto isRegEx = [](const BSONElement& elm) {
                return elm.type() == BSONType::RegEx;
            };

            // Check if all children are equality conditions or regular expressions with the
            // same path argument, and same collation.
            for (auto& childExpression : children) {
                if (childExpression->matchType() != MatchExpression::EQ &&
                    childExpression->matchType() != MatchExpression::REGEX) {
                    ++countNonEquivExpr;
                    continue;
                }

                // Disjunctions of equalities use $eq comparison, which has different semantics from
                // $in for regular expressions. The regex under the equality is matched literally as
                // a string constant, while a regex inside $in is matched as a regular expression.
                // Furthermore, $lookup processing explicitly depends on these different semantics.
                //
                // We should not attempt to rewrite an $eq:<regex> into $in because of these
                // different comparison semantics.
                const CollatorInterface* curCollator = nullptr;
                if (childExpression->matchType() == MatchExpression::EQ) {
                    auto eqExpression =
                        static_cast<EqualityMatchExpression*>(childExpression.get());
                    curCollator = eqExpression->getCollator();
                    if (!eqCollator && curCollator) {
                        eqCollator = curCollator;
                    }
                    if (isRegEx(eqExpression->getData())) {
                        ++countNonEquivExpr;
                        continue;
                    }
                }

                // childExpression is an equality with $in comparison semantics.
                // The current approach assumes there is one (large) group of $eq disjuncts
                // that are on the same path.
                if (!childPath) {
                    // The path of the first equality.
                    childPath = childExpression->path().toString();
                    countEquivEqPaths = 1;
                } else if (*childPath == childExpression->path() &&
                           // Regex ignore collations.
                           (childExpression->matchType() == MatchExpression::REGEX ||
                            // All equalities must have the same collation.
                            eqCollator == curCollator)) {
                    ++countEquivEqPaths;  // subsequent equality on the same path
                } else {
                    ++countNonEquivExpr;  // equality on another path
                }
            }
            tassert(3401201,
                    "All expressions must be classified as either eq-equiv or non-eq-equiv",
                    countEquivEqPaths + countNonEquivExpr == children.size());

            // The condition above checks that there are at least two equalities that can be
            // rewritten to an $in, and the we have classified all $or conditions into two disjoint
            // groups.
            if (countEquivEqPaths > 1) {
                tassert(3401202, "There must be a common path", childPath);
                auto inExpression = std::make_unique<InMatchExpression>(StringData(*childPath));
                auto nonEquivOrExpr =
                    (countNonEquivExpr > 0) ? std::make_unique<OrMatchExpression>() : nullptr;
                BSONArrayBuilder bab;

                for (auto& childExpression : children) {
                    if (*childPath != childExpression->path()) {
                        nonEquivOrExpr->add(std::move(childExpression));
                    } else if (childExpression->matchType() == MatchExpression::EQ) {
                        std::unique_ptr<EqualityMatchExpression> eqExpressionPtr{
                            static_cast<EqualityMatchExpression*>(childExpression.release())};
                        if (isRegEx(eqExpressionPtr->getData()) ||
                            eqExpressionPtr->getCollator() != eqCollator) {
                            nonEquivOrExpr->add(std::move(eqExpressionPtr));
                        } else {
                            bab.append(eqExpressionPtr->getData());
                        }
                    } else if (childExpression->matchType() == MatchExpression::REGEX) {
                        std::unique_ptr<RegexMatchExpression> regexExpressionPtr{
                            static_cast<RegexMatchExpression*>(childExpression.release())};
                        // Reset the path because when we parse a $in expression which contains a
                        // regexp, we create a RegexMatchExpression with an empty path.
                        regexExpressionPtr->setPath({});
                        auto status = inExpression->addRegex(std::move(regexExpressionPtr));
                        tassert(3401203,  // TODO SERVER-53380 convert to tassertStatusOK.
                                "Conversion from OR to IN should always succeed",
                                status == Status::OK());
                    } else {
                        nonEquivOrExpr->add(std::move(childExpression));
                    }
                }
                children.clear();
                tassert(3401204,
                        "Incorrect number of non-equivalent expressions",
                        !nonEquivOrExpr || nonEquivOrExpr->numChildren() == countNonEquivExpr);

                auto backingArr = bab.arr();
                std::vector<BSONElement> inEqualities;
                backingArr.elems(inEqualities);
                tassert(3401205,
                        "Incorrect number of in-equivalent expressions",
                        !countEquivEqPaths ||
                            (inEqualities.size() + inExpression->getRegexes().size()) ==
                                countEquivEqPaths);

                auto status = inExpression->setEqualities(std::move(inEqualities));
                tassert(3401206,  // TODO SERVER-53380 convert to tassertStatusOK.
                        "Conversion from OR to IN should always succeed",
                        status == Status::OK());

                inExpression->setBackingBSON(std::move(backingArr));
                if (eqCollator) {
                    inExpression->setCollator(eqCollator);
                }

                if (countNonEquivExpr > 0) {
                    auto parentOrExpr = std::make_unique<OrMatchExpression>();
                    parentOrExpr->add(std::move(inExpression));
                    if (countNonEquivExpr == 1) {
                        parentOrExpr->add(nonEquivOrExpr->releaseChild(0));
                    } else {
                        parentOrExpr->add(std::move(nonEquivOrExpr));
                    }
                    return parentOrExpr;
                }
                return inExpression;
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

void AndMatchExpression::serialize(BSONObjBuilder* out, SerializationOptions opts) const {
    if (!numChildren()) {
        // It is possible for an AndMatchExpression to have no children, resulting in the serialized
        // expression {$and: []}, which is not a valid query object.
        return;
    }

    BSONArrayBuilder arrBob(out->subarrayStart("$and"));
    _listToBSON(&arrBob, opts);
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

void OrMatchExpression::serialize(BSONObjBuilder* out, SerializationOptions opts) const {
    if (!numChildren()) {
        // It is possible for an OrMatchExpression to have no children, resulting in the serialized
        // expression {$or: []}, which is not a valid query object. An empty $or is logically
        // equivalent to {$alwaysFalse: 1}.
        out->append(AlwaysFalseMatchExpression::kName, 1);
        return;
    }
    BSONArrayBuilder arrBob(out->subarrayStart("$or"));
    _listToBSON(&arrBob, opts);
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

void NorMatchExpression::serialize(BSONObjBuilder* out, SerializationOptions opts) const {
    BSONArrayBuilder arrBob(out->subarrayStart("$nor"));
    _listToBSON(&arrBob, opts);
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
                                                     SerializationOptions opts) {
    BSONObjBuilder childBob;
    exp->serialize(&childBob, opts);
    BSONObj tempObj = childBob.obj();

    BSONArrayBuilder tBob(out->subarrayStart("$nor"));
    tBob.append(tempObj);
    tBob.doneFast();
}

void NotMatchExpression::serialize(BSONObjBuilder* out, SerializationOptions opts) const {
    if (_exp->matchType() == MatchType::AND && _exp->numChildren() == 0) {
        if (opts.replacementForLiteralArgs) {
            out->append("$alwaysFalse", *opts.replacementForLiteralArgs);
        } else {
            out->append("$alwaysFalse", 1);
        }
        return;
    }

    if (!opts.includePath) {
        BSONObjBuilder notBob(out->subobjStart("$not"));
        // Our parser does not accept a $and directly within a $not, instead expecting the direct
        // notation like {x: {$not: {$gt: 5, $lt: 0}}}. We represent such an expression with an AND
        // internally, so we un-nest it here to be able to re-parse it.
        if (_exp->matchType() == MatchType::AND) {
            for (size_t x = 0; x < _exp->numChildren(); ++x) {
                _exp->getChild(x)->serialize(&notBob, opts);
            }
        } else {
            _exp->serialize(&notBob, opts);
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
    //
    // One exception: while TextMatchExpressionBase derives from PathMatchExpression, text match
    // expressions cannot be serialized in the same manner as other PathMatchExpression derivatives.
    // This is because the path for a TextMatchExpression is embedded within the $text object,
    // whereas for other PathMatchExpressions it is on the left-hand-side, for example {x: {$eq:
    // 1}}.
    if (auto pathMatch = dynamic_cast<PathMatchExpression*>(expressionToNegate);
        pathMatch && !dynamic_cast<TextMatchExpressionBase*>(expressionToNegate)) {
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
        notExpression._exp = MatchExpression::optimize(std::move(notExpression._exp));

        return expression;
    };
}
}  // namespace mongo
