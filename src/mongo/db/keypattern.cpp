// @file keypattern.cpp

/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/keypattern.h"

#include "mongo/db/index_names.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

KeyPattern::KeyPattern(const BSONObj& pattern) : _pattern(pattern) {}

bool KeyPattern::isIdKeyPattern(const BSONObj& pattern) {
    BSONObjIterator i(pattern);
    BSONElement e = i.next();
    // _id index must have form exactly {_id : 1} or {_id : -1}.
    // Allows an index of form {_id : "hashed"} to exist but
    // do not consider it to be the primary _id index
    return (0 == strcmp(e.fieldName(), "_id")) && (e.numberInt() == 1 || e.numberInt() == -1) &&
        i.next().eoo();
}

bool KeyPattern::isOrderedKeyPattern(const BSONObj& pattern) {
    return IndexNames::BTREE == IndexNames::findPluginName(pattern);
}

bool KeyPattern::isHashedKeyPattern(const BSONObj& pattern) {
    return IndexNames::HASHED == IndexNames::findPluginName(pattern);
}

BSONObj KeyPattern::extendRangeBound(const BSONObj& bound, bool makeUpperInclusive) const {
    BSONObjBuilder newBound(bound.objsize());

    BSONObjIterator src(bound);
    BSONObjIterator pat(_pattern);

    while (src.more()) {
        massert(16649,
                str::stream() << "keyPattern " << _pattern << " shorter than bound " << bound,
                pat.more());
        BSONElement srcElt = src.next();
        BSONElement patElt = pat.next();
        massert(16634,
                str::stream() << "field names of bound " << bound
                              << " do not match those of keyPattern "
                              << _pattern,
                str::equals(srcElt.fieldName(), patElt.fieldName()));
        newBound.append(srcElt);
    }
    while (pat.more()) {
        BSONElement patElt = pat.next();
        // for non 1/-1 field values, like {a : "hashed"}, treat order as ascending
        int order = patElt.isNumber() ? patElt.numberInt() : 1;
        // flip the order semantics if this is an upper bound
        if (makeUpperInclusive)
            order *= -1;

        if (order > 0) {
            newBound.appendMinKey(patElt.fieldName());
        } else {
            newBound.appendMaxKey(patElt.fieldName());
        }
    }
    return newBound.obj();
}

BSONObj KeyPattern::globalMin() const {
    return extendRangeBound(BSONObj(), false);
}

BSONObj KeyPattern::globalMax() const {
    return extendRangeBound(BSONObj(), true);
}

}  // namespace mongo
