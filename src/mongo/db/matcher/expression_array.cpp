// expression_array.cpp

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

#include "mongo/db/matcher/expression_array.h"

#include "mongo/db/field_ref.h"
#include "mongo/db/jsobj.h"

namespace mongo {

Status ArrayMatchingMatchExpression::setPath(StringData path) {
    _path = path;
    Status s = _elementPath.init(_path);
    _elementPath.setTraverseLeafArray(false);
    return s;
}

bool ArrayMatchingMatchExpression::matches(const MatchableDocument* doc,
                                           MatchDetails* details) const {
    MatchableDocument::IteratorHolder cursor(doc, &_elementPath);

    while (cursor->more()) {
        ElementIterator::Context e = cursor->next();
        if (e.element().type() != Array)
            continue;

        bool amIRoot = e.arrayOffset().eoo();

        if (!matchesArray(e.element().Obj(), amIRoot ? details : NULL))
            continue;

        if (!amIRoot && details && details->needRecord() && !e.arrayOffset().eoo()) {
            details->setElemMatchKey(e.arrayOffset().fieldName());
        }
        return true;
    }
    return false;
}

bool ArrayMatchingMatchExpression::matchesSingleElement(const BSONElement& e) const {
    if (e.type() != Array)
        return false;
    return matchesArray(e.Obj(), NULL);
}


bool ArrayMatchingMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const ArrayMatchingMatchExpression* realOther =
        static_cast<const ArrayMatchingMatchExpression*>(other);

    if (_path != realOther->_path)
        return false;

    if (numChildren() != realOther->numChildren())
        return false;

    for (unsigned i = 0; i < numChildren(); i++)
        if (!getChild(i)->equivalent(realOther->getChild(i)))
            return false;
    return true;
}


// -------

Status ElemMatchObjectMatchExpression::init(StringData path, MatchExpression* sub) {
    _sub.reset(sub);
    return setPath(path);
}

bool ElemMatchObjectMatchExpression::matchesArray(const BSONObj& anArray,
                                                  MatchDetails* details) const {
    BSONObjIterator i(anArray);
    while (i.more()) {
        BSONElement inner = i.next();
        if (!inner.isABSONObj())
            continue;
        if (_sub->matchesBSON(inner.Obj(), NULL)) {
            if (details && details->needRecord()) {
                details->setElemMatchKey(inner.fieldName());
            }
            return true;
        }
    }
    return false;
}

void ElemMatchObjectMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << path() << " $elemMatch (obj)";

    MatchExpression::TagData* td = getTag();
    if (NULL != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
    _sub->debugString(debug, level + 1);
}

void ElemMatchObjectMatchExpression::serialize(BSONObjBuilder* out) const {
    BSONObjBuilder subBob;
    _sub->serialize(&subBob);
    out->append(path(), BSON("$elemMatch" << subBob.obj()));
}


// -------

ElemMatchValueMatchExpression::~ElemMatchValueMatchExpression() {
    for (unsigned i = 0; i < _subs.size(); i++)
        delete _subs[i];
    _subs.clear();
}

Status ElemMatchValueMatchExpression::init(StringData path, MatchExpression* sub) {
    init(path);
    add(sub);
    return Status::OK();
}

Status ElemMatchValueMatchExpression::init(StringData path) {
    return setPath(path);
}


void ElemMatchValueMatchExpression::add(MatchExpression* sub) {
    verify(sub);
    _subs.push_back(sub);
}

bool ElemMatchValueMatchExpression::matchesArray(const BSONObj& anArray,
                                                 MatchDetails* details) const {
    BSONObjIterator i(anArray);
    while (i.more()) {
        BSONElement inner = i.next();

        if (_arrayElementMatchesAll(inner)) {
            if (details && details->needRecord()) {
                details->setElemMatchKey(inner.fieldName());
            }
            return true;
        }
    }
    return false;
}

bool ElemMatchValueMatchExpression::_arrayElementMatchesAll(const BSONElement& e) const {
    for (unsigned i = 0; i < _subs.size(); i++) {
        if (!_subs[i]->matchesSingleElement(e))
            return false;
    }
    return true;
}

void ElemMatchValueMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << path() << " $elemMatch (value)";

    MatchExpression::TagData* td = getTag();
    if (NULL != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
    for (unsigned i = 0; i < _subs.size(); i++) {
        _subs[i]->debugString(debug, level + 1);
    }
}

void ElemMatchValueMatchExpression::serialize(BSONObjBuilder* out) const {
    BSONObjBuilder emBob;

    for (unsigned i = 0; i < _subs.size(); i++) {
        BSONObjBuilder predicate;
        _subs[i]->serialize(&predicate);
        BSONObj predObj = predicate.obj();
        emBob.appendElements(predObj.firstElement().embeddedObject());
    }
    out->append(path(), BSON("$elemMatch" << emBob.obj()));
}


// ---------

Status SizeMatchExpression::init(StringData path, int size) {
    _size = size;
    return setPath(path);
}

bool SizeMatchExpression::matchesArray(const BSONObj& anArray, MatchDetails* details) const {
    if (_size < 0)
        return false;
    return anArray.nFields() == _size;
}

void SizeMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << path() << " $size : " << _size << "\n";

    MatchExpression::TagData* td = getTag();
    if (NULL != td) {
        debug << " ";
        td->debugString(&debug);
    }
}

void SizeMatchExpression::serialize(BSONObjBuilder* out) const {
    out->append(path(), BSON("$size" << _size));
}

bool SizeMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const SizeMatchExpression* realOther = static_cast<const SizeMatchExpression*>(other);
    return path() == realOther->path() && _size == realOther->_size;
}


// ------------------
}
