// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/keypattern.h"

#include "mongo/base/error_codes.h"
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
    return KeyPattern::_addToStringBuilder(sb, keyPattern._pattern);
}

StackStringBuilder& operator<<(StackStringBuilder& sb, const KeyPattern& keyPattern) {
    return KeyPattern::_addToStringBuilder(sb, keyPattern._pattern);
}

template <typename SB>
SB& KeyPattern::_addToStringBuilder(SB& sb, const BSONObj& pattern) {
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
            sb << elem.fieldNameStringData() << ": \"" << elem.valueStringData() << "\"";
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

bool KeyPattern::isGlobalMax(const BSONObj& bound) const {
    if (bound.isEmpty())
        return false;
    BSONObjIterator boundIt(bound);
    BSONObjIterator patternIt(_pattern);
    while (boundIt.more()) {
        massert(ErrorCodes::KeyPatternShorterThanBound,
                str::stream() << "keyPattern " << _pattern << " shorter than bound " << bound,
                patternIt.more());
        BSONElement boundElem = boundIt.next();
        BSONElement patternElem = patternIt.next();
        massert(12153300,
                str::stream() << "field names of bound " << bound
                              << " do not match those of keyPattern " << _pattern,
                boundElem.fieldNameStringData() == patternElem.fieldNameStringData());
        if (boundElem.type() != BSONType::maxKey)
            return false;
    }
    return true;
}

size_t KeyPattern::getApproximateSize() const {
    auto size = sizeof(KeyPattern);
    size += _pattern.isOwned() ? _pattern.objsize() : 0;
    return size;
}

}  // namespace mongo
