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

namespace mongo {

ListOfMatchExpression::~ListOfMatchExpression() {
    for (unsigned i = 0; i < _expressions.size(); i++)
        delete _expressions[i];
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

bool AndMatchExpression::matchesSingleElement(const BSONElement& e) const {
    for (size_t i = 0; i < numChildren(); i++) {
        if (!getChild(i)->matchesSingleElement(e)) {
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

bool OrMatchExpression::matchesSingleElement(const BSONElement& e) const {
    for (size_t i = 0; i < numChildren(); i++) {
        if (getChild(i)->matchesSingleElement(e)) {
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

bool NorMatchExpression::matchesSingleElement(const BSONElement& e) const {
    for (size_t i = 0; i < numChildren(); i++) {
        if (getChild(i)->matchesSingleElement(e)) {
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
}
