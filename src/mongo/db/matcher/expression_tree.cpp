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

#include "mongo/db/matcher/expression_tree.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_path.h"
#include "mongo/db/matcher/expression_text_base.h"

namespace mongo {

ListOfMatchExpression::~ListOfMatchExpression() {
    for (unsigned i = 0; i < _expressions.size(); i++) {
        delete _expressions[i];
    }
    _expressions.clear();
}

void ListOfMatchExpression::add(MatchExpression* e) {
    verify(e);
    _expressions.push_back(e);
}


void ListOfMatchExpression::_debugList(StringBuilder& debug, int indentationLevel) const {
    for (unsigned i = 0; i < _expressions.size(); i++)
        _expressions[i]->debugString(debug, indentationLevel + 1);
}

void ListOfMatchExpression::_listToBSON(BSONArrayBuilder* out, bool includePath) const {
    for (unsigned i = 0; i < _expressions.size(); i++) {
        BSONObjBuilder childBob(out->subobjStart());
        _expressions[i]->serialize(&childBob, includePath);
    }
    out->doneFast();
}

MatchExpression::ExpressionOptimizerFunc ListOfMatchExpression::getOptimizer() const {
    return [](std::unique_ptr<MatchExpression> expression) -> std::unique_ptr<MatchExpression> {
        auto& children = static_cast<ListOfMatchExpression&>(*expression)._expressions;

        // Recursively apply optimizations to child expressions.
        for (auto& childExpression : children) {
            // Since 'childExpression' is a reference to a member of the ListOfMatchExpression's
            // child array, this assignment replaces the original child with the optimized child.
            // We must set this child's entry in '_expressions' to null after assigning ownership to
            // 'childExpressionPtr'. Otherwise, if the call to optimize() throws we will attempt to
            // free twice.
            std::unique_ptr<MatchExpression> childExpressionPtr(childExpression);
            childExpression = nullptr;

            auto optimizedExpression = MatchExpression::optimize(std::move(childExpressionPtr));
            childExpression = optimizedExpression.release();
        }

        // Associativity of AND and OR: an AND absorbs the children of any ANDs among its children
        // (and likewise for any OR with OR children).
        MatchType matchType = expression->matchType();
        if (matchType == AND || matchType == OR) {
            std::vector<MatchExpression*> absorbedExpressions;
            for (MatchExpression*& childExpression : children) {
                if (childExpression->matchType() == matchType) {
                    // Move this child out of the children array.
                    std::unique_ptr<ListOfMatchExpression> childExpressionPtr(
                        static_cast<ListOfMatchExpression*>(childExpression));
                    childExpression = nullptr;  // Null out this child's entry in _expressions, so
                                                // that it will be deleted by the erase call below.

                    // Move all of the grandchildren from the child expression to
                    // absorbedExpressions.
                    auto& grandChildren = childExpressionPtr->_expressions;
                    absorbedExpressions.insert(
                        absorbedExpressions.end(), grandChildren.begin(), grandChildren.end());
                    grandChildren.clear();

                    // Note that 'childExpressionPtr' will now be destroyed.
                }
            }

            // We replaced each destroyed child expression with nullptr. Now we remove those
            // nullptrs from the array.
            children.erase(std::remove(children.begin(), children.end(), nullptr), children.end());

            // Append the absorbed children to the end of the array.
            children.insert(children.end(), absorbedExpressions.begin(), absorbedExpressions.end());
        }

        // Remove all children of AND that are $alwaysTrue and all children of OR that are
        // $alwaysFalse.
        if (matchType == AND || matchType == OR) {
            for (MatchExpression*& childExpression : children) {
                if ((childExpression->isTriviallyTrue() && matchType == MatchExpression::AND) ||
                    (childExpression->isTriviallyFalse() && matchType == MatchExpression::OR)) {
                    std::unique_ptr<MatchExpression> childPtr(childExpression);
                    childExpression = nullptr;
                }
            }

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
                MatchExpression* simplifiedExpression = children.front();
                children.clear();
                return std::unique_ptr<MatchExpression>(simplifiedExpression);
            } else if (matchType == NOR) {
                // Simplify NOR of exactly one operand to NOT of that operand.
                auto simplifiedExpression = std::make_unique<NotMatchExpression>(children.front());
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
        if (!_expressions[i]->equivalent(realOther->_expressions[i]))
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
    MatchExpression::TagData* td = getTag();
    if (td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
    _debugList(debug, indentationLevel);
}

void AndMatchExpression::serialize(BSONObjBuilder* out, bool includePath) const {
    if (!numChildren()) {
        // It is possible for an AndMatchExpression to have no children, resulting in the serialized
        // expression {$and: []}, which is not a valid query object.
        return;
    }

    BSONArrayBuilder arrBob(out->subarrayStart("$and"));
    _listToBSON(&arrBob, includePath);
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
    for (size_t i = 0; i < numChildren(); i++) {
        if (getChild(i)->matchesSingleElement(e, details)) {
            return true;
        }
    }
    return false;
}


void OrMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << "$or";
    MatchExpression::TagData* td = getTag();
    if (td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
    _debugList(debug, indentationLevel);
}

void OrMatchExpression::serialize(BSONObjBuilder* out, bool includePath) const {
    if (!numChildren()) {
        // It is possible for an OrMatchExpression to have no children, resulting in the serialized
        // expression {$or: []}, which is not a valid query object. An empty $or is logically
        // equivalent to {$alwaysFalse: 1}.
        out->append(AlwaysFalseMatchExpression::kName, 1);
        return;
    }
    BSONArrayBuilder arrBob(out->subarrayStart("$or"));
    _listToBSON(&arrBob, includePath);
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
    debug << "$nor\n";
    _debugList(debug, indentationLevel);
}

void NorMatchExpression::serialize(BSONObjBuilder* out, bool includePath) const {
    BSONArrayBuilder arrBob(out->subarrayStart("$nor"));
    _listToBSON(&arrBob, includePath);
}

// -------

void NotMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << "$not\n";
    _exp->debugString(debug, indentationLevel + 1);
}

void NotMatchExpression::serializeNotExpressionToNor(MatchExpression* exp,
                                                     BSONObjBuilder* out,
                                                     bool includePath) {
    BSONObjBuilder childBob;
    exp->serialize(&childBob, includePath);
    BSONObj tempObj = childBob.obj();

    BSONArrayBuilder tBob(out->subarrayStart("$nor"));
    tBob.append(tempObj);
    tBob.doneFast();
}

void NotMatchExpression::serialize(BSONObjBuilder* out, bool includePath) const {
    if (_exp->matchType() == MatchType::AND && _exp->numChildren() == 0) {
        out->append("$alwaysFalse", 1);
        return;
    }

    if (!includePath) {
        BSONObjBuilder notBob(out->subobjStart("$not"));
        // Our parser does not accept a $and directly within a $not, instead expecting the direct
        // notation like {x: {$not: {$gt: 5, $lt: 0}}}. We represent such an expression with an AND
        // internally, so we un-nest it here to be able to re-parse it.
        if (_exp->matchType() == MatchType::AND) {
            for (size_t x = 0; x < _exp->numChildren(); ++x) {
                _exp->getChild(x)->serialize(&notBob, includePath);
            }
        } else {
            _exp->serialize(&notBob, includePath);
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
        const auto path = pathMatch->path();
        BSONObjBuilder pathBob(out->subobjStart(path));
        pathBob.append("$not", pathMatch->getSerializedRightHandSide());
        return;
    }
    return serializeNotExpressionToNor(expressionToNegate, out, includePath);
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
