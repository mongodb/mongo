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

#include "mongo/db/keypattern.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/index_names.h"
#include "mongo/util/assert_util.h"

namespace mongo {

KeyPattern::KeyPattern(const BSONObj& pattern) : _pattern(pattern) {}

bool KeyPattern::isOrderedKeyPattern(const BSONObj& pattern) {
    return IndexNames::BTREE == IndexNames::findPluginName(pattern);
}

bool KeyPattern::isHashedKeyPattern(const BSONObj& pattern) {
    return IndexNames::HASHED == IndexNames::findPluginName(pattern);
}

StringBuilder& operator<<(StringBuilder& sb, const KeyPattern& keyPattern) {
    return KeyPattern::addToStringBuilder(sb, keyPattern._pattern);
}

StringBuilder& KeyPattern::addToStringBuilder(StringBuilder& sb, const BSONObj& pattern) {
    // Rather than return BSONObj::toString() we construct a keyPattern string manually. This allows
    // us to avoid the cost of writing numeric direction to the str::stream which will then undergo
    // expensive number to string conversion.
    sb << "{ ";

    bool first = true;
    for (auto&& elem : pattern) {
        if (first) {
            first = false;
        } else {
            sb << ", ";
        }

        if (BSONType::string == elem.type()) {
            sb << elem;
        } else if (elem.number() >= 0) {
            // The canonical check as to whether a key pattern element is "ascending" or
            // "descending" is (elem.number() >= 0). This is defined by the Ordering class.
            sb << elem.fieldNameStringData() << ": 1";
        } else {
            sb << elem.fieldNameStringData() << ": -1";
        }
    }

    sb << " }";
    return sb;
}

BSONObj KeyPattern::extendRangeBound(const BSONObj& bound, bool makeUpperInclusive) const {
    BSONObjBuilder newBound(bound.objsize());

    BSONObjIterator src(bound);
    BSONObjIterator pat(_pattern);

    while (src.more()) {
        massert(ErrorCodes::KeyPatternShorterThanBound,
                str::stream() << "keyPattern " << _pattern << " shorter than bound " << bound,
                pat.more());
        BSONElement srcElt = src.next();
        BSONElement patElt = pat.next();
        massert(16634,
                str::stream() << "field names of bound " << bound
                              << " do not match those of keyPattern " << _pattern,
                srcElt.fieldNameStringData() == patElt.fieldNameStringData());
        newBound.append(srcElt);
    }
    while (pat.more()) {
        BSONElement patElt = pat.next();
        // for non 1/-1 field values, like {a : "hashed"}, treat order as ascending
        int order = patElt.isNumber() ? patElt.safeNumberInt() : 1;
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

size_t KeyPattern::getApproximateSize() const {
    auto size = sizeof(KeyPattern);
    size += _pattern.isOwned() ? _pattern.objsize() : 0;
    return size;
}

}  // namespace mongo
