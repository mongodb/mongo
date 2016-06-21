// expression_leaf.h

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

#pragma once

#include <unordered_map>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/stdx/memory.h"

namespace pcrecpp {
class RE;
}  // namespace pcrecpp;

namespace mongo {

class CollatorInterface;

/**
 * This file contains leaves in the parse tree that are not array-based.
 *
 * LeafMatchExpression: REGEX MOD EXISTS MATCH_IN
 * ComparisonMatchExpression: EQ LTE LT GT GTE
 * MatchExpression: TYPE_OPERATOR
 */

/**
 * Many operators subclass from this:
 * REGEX, MOD, EXISTS, IN
 * Everything that inherits from ComparisonMatchExpression.
 */
class LeafMatchExpression : public MatchExpression {
public:
    LeafMatchExpression(MatchType matchType) : MatchExpression(matchType) {}

    virtual ~LeafMatchExpression() {}

    virtual bool matches(const MatchableDocument* doc, MatchDetails* details = 0) const;

    virtual bool matchesSingleElement(const BSONElement& e) const = 0;

    virtual const StringData path() const {
        return _path;
    }

    Status setPath(StringData path);

private:
    StringData _path;
    ElementPath _elementPath;
};

/**
 * EQ, LTE, LT, GT, GTE subclass from ComparisonMatchExpression.
 */
class ComparisonMatchExpression : public LeafMatchExpression {
public:
    ComparisonMatchExpression(MatchType type) : LeafMatchExpression(type) {}

    Status init(StringData path, const BSONElement& rhs);

    virtual ~ComparisonMatchExpression() {}

    virtual bool matchesSingleElement(const BSONElement& e) const;

    virtual const BSONElement& getRHS() const {
        return _rhs;
    }

    virtual void debugString(StringBuilder& debug, int level = 0) const;

    /**
     * 'collator' must outlive the ComparisonMatchExpression and any clones made of it.
     */
    virtual void _doSetCollator(const CollatorInterface* collator) {
        _collator = collator;
    }

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    const BSONElement& getData() const {
        return _rhs;
    }

    const CollatorInterface* getCollator() const {
        return _collator;
    }

    /**
     * Returns true if the MatchExpression is a ComparisonMatchExpression.
     */
    static bool isComparisonMatchExpression(const MatchExpression* expr) {
        switch (expr->matchType()) {
            case MatchExpression::LT:
            case MatchExpression::LTE:
            case MatchExpression::EQ:
            case MatchExpression::GTE:
            case MatchExpression::GT:
                return true;
            default:
                return false;
        }
    }

protected:
    BSONElement _rhs;

    // Collator used to compare elements. By default, simple binary comparison will be used.
    const CollatorInterface* _collator = nullptr;
};

//
// ComparisonMatchExpression inheritors
//

class EqualityMatchExpression : public ComparisonMatchExpression {
public:
    EqualityMatchExpression() : ComparisonMatchExpression(EQ) {}
    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e = stdx::make_unique<EqualityMatchExpression>();
        e->init(path(), _rhs);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

class LTEMatchExpression : public ComparisonMatchExpression {
public:
    LTEMatchExpression() : ComparisonMatchExpression(LTE) {}
    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e = stdx::make_unique<LTEMatchExpression>();
        e->init(path(), _rhs);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

class LTMatchExpression : public ComparisonMatchExpression {
public:
    LTMatchExpression() : ComparisonMatchExpression(LT) {}
    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e = stdx::make_unique<LTMatchExpression>();
        e->init(path(), _rhs);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

class GTMatchExpression : public ComparisonMatchExpression {
public:
    GTMatchExpression() : ComparisonMatchExpression(GT) {}
    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e = stdx::make_unique<GTMatchExpression>();
        e->init(path(), _rhs);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

class GTEMatchExpression : public ComparisonMatchExpression {
public:
    GTEMatchExpression() : ComparisonMatchExpression(GTE) {}
    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e = stdx::make_unique<GTEMatchExpression>();
        e->init(path(), _rhs);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

//
// LeafMatchExpression inheritors
//

class RegexMatchExpression : public LeafMatchExpression {
public:
    /**
     * Maximum pattern size which pcre v8.3 can do matches correctly with
     * LINK_SIZE define macro set to 2 @ pcre's config.h (based on
     * experiments)
     */
    static const size_t MaxPatternSize = 32764;

    RegexMatchExpression();
    ~RegexMatchExpression();

    Status init(StringData path, StringData regex, StringData options);
    Status init(StringData path, const BSONElement& e);

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<RegexMatchExpression> e = stdx::make_unique<RegexMatchExpression>();
        e->init(path(), _regex, _flags);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return std::move(e);
    }

    virtual bool matchesSingleElement(const BSONElement& e) const;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    void shortDebugString(StringBuilder& debug) const;

    virtual bool equivalent(const MatchExpression* other) const;

    const std::string& getString() const {
        return _regex;
    }
    const std::string& getFlags() const {
        return _flags;
    }

private:
    std::string _regex;
    std::string _flags;
    std::unique_ptr<pcrecpp::RE> _re;
};

class ModMatchExpression : public LeafMatchExpression {
public:
    ModMatchExpression() : LeafMatchExpression(MOD) {}

    Status init(StringData path, int divisor, int remainder);

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ModMatchExpression> m = stdx::make_unique<ModMatchExpression>();
        m->init(path(), _divisor, _remainder);
        if (getTag()) {
            m->setTag(getTag()->clone());
        }
        return std::move(m);
    }

    virtual bool matchesSingleElement(const BSONElement& e) const;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    int getDivisor() const {
        return _divisor;
    }
    int getRemainder() const {
        return _remainder;
    }

private:
    int _divisor;
    int _remainder;
};

class ExistsMatchExpression : public LeafMatchExpression {
public:
    ExistsMatchExpression() : LeafMatchExpression(EXISTS) {}

    Status init(StringData path);

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ExistsMatchExpression> e = stdx::make_unique<ExistsMatchExpression>();
        e->init(path());
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return std::move(e);
    }

    virtual bool matchesSingleElement(const BSONElement& e) const;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;
};

/**
 * query operator: $in
 */
class InMatchExpression : public LeafMatchExpression {
public:
    InMatchExpression() : LeafMatchExpression(MATCH_IN) {}

    Status init(StringData path);

    virtual std::unique_ptr<MatchExpression> shallowClone() const;

    virtual bool matchesSingleElement(const BSONElement& e) const;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    /**
     * 'collator' must outlive the InMatchExpression and any clones made of it.
     */
    virtual void _doSetCollator(const CollatorInterface* collator);

    Status addEquality(const BSONElement& elt);

    Status addRegex(std::unique_ptr<RegexMatchExpression> expr);

    const BSONElementSet& getEqualities() const {
        return _equalitySet;
    }

    const std::vector<std::unique_ptr<RegexMatchExpression>>& getRegexes() const {
        return _regexes;
    }

    const CollatorInterface* getCollator() const {
        return _collator;
    }

    bool hasNull() const {
        return _hasNull;
    }

    bool hasEmptyArray() const {
        return _hasEmptyArray;
    }

private:
    // Whether or not '_equalities' has a jstNULL element in it.
    bool _hasNull = false;

    // Whether or not '_equalities' has an empty array element in it.
    bool _hasEmptyArray = false;

    // Collator used to compare elements. By default, simple binary comparison will be used.
    const CollatorInterface* _collator = nullptr;

    // Set of equality elements associated with this expression. '_collator' is used as a comparator
    // for this set.
    BSONElementSet _equalitySet;

    // Original container of equality elements, including duplicates. Needed for re-computing
    // '_equalitySet' in case '_collator' changes after elements have been added.
    std::vector<BSONElement> _originalEqualityVector;

    // Container of regex elements this object owns.
    std::vector<std::unique_ptr<RegexMatchExpression>> _regexes;
};

//
// The odd duck out, TYPE_OPERATOR.
//

/**
 * Type has some odd semantics with arrays and as such it can't inherit from
 * LeafMatchExpression.
 */
class TypeMatchExpression : public MatchExpression {
public:
    static const std::string kMatchesAllNumbersAlias;
    static const std::unordered_map<std::string, BSONType> typeAliasMap;

    TypeMatchExpression() : MatchExpression(TYPE_OPERATOR) {}

    /**
     * Initialize as matching against a specific BSONType.
     *
     * Returns a non-OK status if 'type' cannot be converted to a valid BSONType.
     */
    Status initWithBSONType(StringData path, int type);

    /**
     * Initialize as matching against all number types (NumberDouble, NumberLong, and NumberInt).
     */
    Status initAsMatchingAllNumbers(StringData path);

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<TypeMatchExpression> e = stdx::make_unique<TypeMatchExpression>();
        if (_matchesAllNumbers) {
            e->initAsMatchingAllNumbers(_path);
        } else {
            e->initWithBSONType(_path, _type);
        }
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return std::move(e);
    }

    virtual bool matchesSingleElement(const BSONElement& e) const;

    virtual bool matches(const MatchableDocument* doc, MatchDetails* details = 0) const;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    /**
     * What is the type we're matching against?
     */
    BSONType getType() const {
        return _type;
    }

    /**
     * Whether or not to match against all number types (NumberDouble, NumberLong, and NumberInt).
     * Defaults to false. If this is true, _type is EOO.
     */
    bool matchesAllNumbers() const {
        return _matchesAllNumbers;
    }

    virtual const StringData path() const {
        return _path;
    }

private:
    bool _matches(StringData path, const MatchableDocument* doc, MatchDetails* details = 0) const;

    StringData _path;
    ElementPath _elementPath;
    bool _matchesAllNumbers = false;
    BSONType _type = BSONType::EOO;
};

/**
 * Bit test query operators include $bitsAllSet, $bitsAllClear, $bitsAnySet, and $bitsAnyClear.
 */
class BitTestMatchExpression : public LeafMatchExpression {
public:
    // Constant used in matchesSingleElement() and MatchExpressionParser::_parseBitTest. Is a
    // double representation of 2^63.
    static const double kLongLongMaxPlusOneAsDouble;

    BitTestMatchExpression(MatchType type) : LeafMatchExpression(type) {}
    virtual ~BitTestMatchExpression() {}

    /**
     * Initialize with either bit positions, a 64-bit numeric bitmask, or a char array
     * bitmask.
     */
    Status init(StringData path, std::vector<uint32_t> bitPositions);
    Status init(StringData path, uint64_t bitMask);
    Status init(StringData path, const char* bitMaskBinary, uint32_t bitMaskLen);

    virtual bool matchesSingleElement(const BSONElement& e) const;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    size_t numBitPositions() const {
        return _bitPositions.size();
    }

    const std::vector<uint32_t>& getBitPositions() const {
        return _bitPositions;
    }

protected:
    /**
     * Used to copy this match expression to another BitTestMatchExpression. Does not take
     * ownership.
     */
    void initClone(BitTestMatchExpression* clone) const {
        clone->init(path(), _bitPositions);
        if (getTag()) {
            clone->setTag(getTag()->clone());
        }
    }

private:
    /**
     * Performs bit test using bit positions on 'eValue' and returns whether or not the bit test
     * passes.
     */
    bool performBitTest(long long eValue) const;

    /**
     * Performs bit test using bit positions on 'eBinary' with length (in bytes) 'eBinaryLen' and
     * returns whether or not the bit test passes.
     */
    bool performBitTest(const char* eBinary, uint32_t eBinaryLen) const;

    /**
     * Helper function for performBitTest(...).
     *
     * needFurtherBitTests() determines if the result of a bit-test ('isBitSet') is enough
     * information to skip the rest of the bit tests.
     **/
    bool needFurtherBitTests(bool isBitSet) const;

    // Vector of bit positions to test, with bit position 0 being the least significant bit.
    // Used to perform bit tests against BinData.
    std::vector<uint32_t> _bitPositions;

    // Used to perform bit tests against numbers using a single bitwise operation.
    uint64_t _bitMask = 0;
};

class BitsAllSetMatchExpression : public BitTestMatchExpression {
public:
    BitsAllSetMatchExpression() : BitTestMatchExpression(BITS_ALL_SET) {}
    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<BitTestMatchExpression> bitTestMatchExpression =
            stdx::make_unique<BitsAllSetMatchExpression>();
        initClone(bitTestMatchExpression.get());
        return std::move(bitTestMatchExpression);
    }
};

class BitsAllClearMatchExpression : public BitTestMatchExpression {
public:
    BitsAllClearMatchExpression() : BitTestMatchExpression(BITS_ALL_CLEAR) {}
    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<BitTestMatchExpression> bitTestMatchExpression =
            stdx::make_unique<BitsAllClearMatchExpression>();
        initClone(bitTestMatchExpression.get());
        return std::move(bitTestMatchExpression);
    }
};

class BitsAnySetMatchExpression : public BitTestMatchExpression {
public:
    BitsAnySetMatchExpression() : BitTestMatchExpression(BITS_ANY_SET) {}
    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<BitTestMatchExpression> bitTestMatchExpression =
            stdx::make_unique<BitsAnySetMatchExpression>();
        initClone(bitTestMatchExpression.get());
        return std::move(bitTestMatchExpression);
    }
};

class BitsAnyClearMatchExpression : public BitTestMatchExpression {
public:
    BitsAnyClearMatchExpression() : BitTestMatchExpression(BITS_ANY_CLEAR) {}
    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<BitTestMatchExpression> bitTestMatchExpression =
            stdx::make_unique<BitsAnyClearMatchExpression>();
        initClone(bitTestMatchExpression.get());
        return std::move(bitTestMatchExpression);
    }
};

}  // namespace mongo
