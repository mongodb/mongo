// expression_tree.cpp

/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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


void ListOfMatchExpression::_debugList(StringBuilder& debug, int level) const {
    for (unsigned i = 0; i < _expressions.size(); i++)
        _expressions[i]->debugString(debug, level + 1);
}

void ListOfMatchExpression::_listToBSON(BSONArrayBuilder* out) const {
    for (unsigned i = 0; i < _expressions.size(); i++) {
        BSONObjBuilder childBob(out->subobjStart());
        _expressions[i]->serialize(&childBob);
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

        if (children.size() == 1) {
            if ((matchType == AND || matchType == OR || matchType == INTERNAL_SCHEMA_XOR)) {
                // Simplify AND/OR/XOR with exactly one operand to an expression consisting of just
                // that operand.
                MatchExpression* simplifiedExpression = children.front();
                children.clear();
                return std::unique_ptr<MatchExpression>(simplifiedExpression);
            } else if (matchType == NOR) {
                // Simplify NOR of exactly one operand to NOT of that operand.
                auto simplifiedExpression = stdx::make_unique<NotMatchExpression>();
                invariantOK(simplifiedExpression->init(children.front()));
                children.clear();
                return std::move(simplifiedExpression);
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


void AndMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << "$and\n";
    _debugList(debug, level);
}

void AndMatchExpression::serialize(BSONObjBuilder* out) const {
    if (!numChildren()) {
        // It is possible for an AndMatchExpression to have no children, resulting in the serialized
        // expression {$and: []}, which is not a valid query object.
        return;
    }

    BSONArrayBuilder arrBob(out->subarrayStart("$and"));
    _listToBSON(&arrBob);
    arrBob.doneFast();
}

// -----

bool OrMatchExpression::matches(const MatchableDocument* doc, MatchDetails* details) const {
    for (size_t i = 0; i < numChildren(); i++) {
        if (getChild(i)->matches(doc, NULL)) {
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


void OrMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << "$or\n";
    _debugList(debug, level);
}

void OrMatchExpression::serialize(BSONObjBuilder* out) const {
    if (!numChildren()) {
        out->append(AlwaysFalseMatchExpression::kName, 1);
        return;
    }
    BSONArrayBuilder arrBob(out->subarrayStart("$or"));
    _listToBSON(&arrBob);
}

// ----

bool NorMatchExpression::matches(const MatchableDocument* doc, MatchDetails* details) const {
    for (size_t i = 0; i < numChildren(); i++) {
        if (getChild(i)->matches(doc, NULL)) {
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

void NorMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << "$nor\n";
    _debugList(debug, level);
}

void NorMatchExpression::serialize(BSONObjBuilder* out) const {
    BSONArrayBuilder arrBob(out->subarrayStart("$nor"));
    _listToBSON(&arrBob);
}

// -------

void NotMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << "$not\n";
    _exp->debugString(debug, level + 1);
}

void NotMatchExpression::serialize(BSONObjBuilder* out) const {
    BSONObjBuilder childBob;
    _exp->serialize(&childBob);

    BSONObj tempObj = childBob.obj();

    // We don't know what the inner object is, and thus whether serializing to $not will result in a
    // parseable MatchExpression. As a fix, we change it to $nor, which is always parseable.
    BSONArrayBuilder tBob(out->subarrayStart("$nor"));
    tBob.append(tempObj);
    tBob.doneFast();
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
}
