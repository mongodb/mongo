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

#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_path.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/unordered_map.h"

namespace pcrecpp {
class RE;
}  // namespace pcrecpp;

namespace mongo {

class CollatorInterface;

class LeafMatchExpression : public PathMatchExpression {
public:
    LeafMatchExpression(MatchType matchType)
        : LeafMatchExpression(matchType,
                              ElementPath::LeafArrayBehavior::kTraverse,
                              ElementPath::NonLeafArrayBehavior::kTraverse) {}

    LeafMatchExpression(MatchType matchType,
                        ElementPath::LeafArrayBehavior leafArrBehavior,
                        ElementPath::NonLeafArrayBehavior nonLeafArrBehavior)
        : PathMatchExpression(matchType, leafArrBehavior, nonLeafArrBehavior) {}

    virtual ~LeafMatchExpression() = default;

    size_t numChildren() const override {
        return 0;
    }

    MatchExpression* getChild(size_t i) const override {
        MONGO_UNREACHABLE;
    }

    std::vector<MatchExpression*>* getChildVector() override {
        return nullptr;
    }

    MatchCategory getCategory() const override {
        return MatchCategory::kLeaf;
    }
};

/**
 * Base class for comparison-like match expression nodes. This includes both the comparison nodes in
 * the match language ($eq, $gt, $gte, $lt, and $lte), as well as internal comparison nodes like
 * $_internalExprEq.
 */
class ComparisonMatchExpressionBase : public LeafMatchExpression {
public:
    static bool isEquality(MatchType matchType) {
        switch (matchType) {
            case MatchExpression::EQ:
            case MatchExpression::INTERNAL_EXPR_EQ:
                return true;
            default:
                return false;
        }
    }

    ComparisonMatchExpressionBase(MatchType type,
                                  ElementPath::LeafArrayBehavior leafArrBehavior,
                                  ElementPath::NonLeafArrayBehavior nonLeafArrBehavior)
        : LeafMatchExpression(type, leafArrBehavior, nonLeafArrBehavior) {}

    virtual ~ComparisonMatchExpressionBase() = default;

    virtual void debugString(StringBuilder& debug, int level = 0) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    /**
     * Returns the name of this MatchExpression.
     */
    virtual StringData name() const = 0;

    const BSONElement& getData() const {
        return _rhs;
    }

    const CollatorInterface* getCollator() const {
        return _collator;
    }

protected:
    /**
     * 'collator' must outlive the ComparisonMatchExpression and any clones made of it.
     */
    virtual void _doSetCollator(const CollatorInterface* collator) {
        _collator = collator;
    }

    BSONElement _rhs;

    // Collator used to compare elements. By default, simple binary comparison will be used.
    const CollatorInterface* _collator = nullptr;

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }
};

/**
 * EQ, LTE, LT, GT, GTE subclass from ComparisonMatchExpression.
 */
class ComparisonMatchExpression : public ComparisonMatchExpressionBase {
public:
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

    explicit ComparisonMatchExpression(MatchType type)
        : ComparisonMatchExpressionBase(type,
                                        ElementPath::LeafArrayBehavior::kTraverse,
                                        ElementPath::NonLeafArrayBehavior::kTraverse) {}

    virtual ~ComparisonMatchExpression() = default;

    Status init(StringData path, BSONElement rhs);

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;
};

class EqualityMatchExpression final : public ComparisonMatchExpression {
public:
    static constexpr StringData kName = "$eq"_sd;

    EqualityMatchExpression() : ComparisonMatchExpression(EQ) {}

    StringData name() const final {
        return kName;
    }

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e = stdx::make_unique<EqualityMatchExpression>();
        invariantOK(e->init(path(), _rhs));
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

class LTEMatchExpression final : public ComparisonMatchExpression {
public:
    static constexpr StringData kName = "$lte"_sd;

    LTEMatchExpression() : ComparisonMatchExpression(LTE) {}

    StringData name() const final {
        return kName;
    }

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e = stdx::make_unique<LTEMatchExpression>();
        invariantOK(e->init(path(), _rhs));
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

class LTMatchExpression final : public ComparisonMatchExpression {
public:
    static constexpr StringData kName = "$lt"_sd;

    LTMatchExpression() : ComparisonMatchExpression(LT) {}

    StringData name() const final {
        return kName;
    }

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e = stdx::make_unique<LTMatchExpression>();
        invariantOK(e->init(path(), _rhs));
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

class GTMatchExpression final : public ComparisonMatchExpression {
public:
    static constexpr StringData kName = "$gt"_sd;

    GTMatchExpression() : ComparisonMatchExpression(GT) {}

    StringData name() const final {
        return kName;
    }

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e = stdx::make_unique<GTMatchExpression>();
        invariantOK(e->init(path(), _rhs));
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

class GTEMatchExpression : public ComparisonMatchExpression {
public:
    static constexpr StringData kName = "$gte"_sd;

    GTEMatchExpression() : ComparisonMatchExpression(GTE) {}

    StringData name() const final {
        return kName;
    }

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ComparisonMatchExpression> e = stdx::make_unique<GTEMatchExpression>();
        invariantOK(e->init(path(), _rhs));
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        e->setCollator(_collator);
        return std::move(e);
    }
};

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
        invariantOK(e->init(path(), _regex, _flags));
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return std::move(e);
    }

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    void serializeToBSONTypeRegex(BSONObjBuilder* out) const;

    void shortDebugString(StringBuilder& debug) const;

    virtual bool equivalent(const MatchExpression* other) const;

    const std::string& getString() const {
        return _regex;
    }
    const std::string& getFlags() const {
        return _flags;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

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
        invariantOK(m->init(path(), _divisor, _remainder));
        if (getTag()) {
            m->setTag(getTag()->clone());
        }
        return std::move(m);
    }

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

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
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

    int _divisor;
    int _remainder;
};

class ExistsMatchExpression : public LeafMatchExpression {
public:
    ExistsMatchExpression() : LeafMatchExpression(EXISTS) {}

    Status init(StringData path);

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        std::unique_ptr<ExistsMatchExpression> e = stdx::make_unique<ExistsMatchExpression>();
        invariantOK(e->init(path()));
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return std::move(e);
    }

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }
};

/**
 * query operator: $in
 */
class InMatchExpression : public LeafMatchExpression {
public:
    InMatchExpression()
        : LeafMatchExpression(MATCH_IN),
          _eltCmp(BSONElementComparator::FieldNamesMode::kIgnore, _collator),
          _equalitySet(_eltCmp.makeBSONEltFlatSet(_originalEqualityVector)) {}

    Status init(StringData path);

    virtual std::unique_ptr<MatchExpression> shallowClone() const;

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual void debugString(StringBuilder& debug, int level) const;

    virtual void serialize(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    /**
     * 'collator' must outlive the InMatchExpression and any clones made of it.
     */
    virtual void _doSetCollator(const CollatorInterface* collator);

    Status setEqualities(std::vector<BSONElement> equalities);

    Status addRegex(std::unique_ptr<RegexMatchExpression> expr);

    const BSONEltFlatSet& getEqualities() const {
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
    ExpressionOptimizerFunc getOptimizer() const final;

    // Whether or not '_equalities' has a jstNULL element in it.
    bool _hasNull = false;

    // Whether or not '_equalities' has an empty array element in it.
    bool _hasEmptyArray = false;

    // Collator used to construct '_eltCmp';
    const CollatorInterface* _collator = nullptr;

    // Comparator used to compare elements. By default, simple binary comparison will be used.
    BSONElementComparator _eltCmp;

    // Original container of equality elements, including duplicates. Needed for re-computing
    // '_equalitySet' in case '_collator' changes after elements have been added.
    //
    // We keep the equalities in sorted order according to the current BSON element comparator. This
    // list of equalities will be used to construct a boost::flat_set, which maintains the set of
    // elements in sorted order within a contiguous region of memory. Sorting and then constructing
    // a flat_set is O(n log n), whereas the boost::flat_set constructor is O(n ^ 2) due to
    // https://svn.boost.org/trac10/ticket/13140.
    std::vector<BSONElement> _originalEqualityVector;

    // Set of equality elements associated with this expression. '_eltCmp' is used as a comparator
    // for this set.
    BSONEltFlatSet _equalitySet;

    // Container of regex elements this object owns.
    std::vector<std::unique_ptr<RegexMatchExpression>> _regexes;
};

/**
 * Bit test query operators include $bitsAllSet, $bitsAllClear, $bitsAnySet, and $bitsAnyClear.
 */
class BitTestMatchExpression : public LeafMatchExpression {
public:
    explicit BitTestMatchExpression(MatchType type) : LeafMatchExpression(type) {}
    virtual ~BitTestMatchExpression() {}

    /**
     * Initialize with either bit positions, a 64-bit numeric bitmask, or a char array
     * bitmask.
     */
    Status init(StringData path, std::vector<uint32_t> bitPositions);
    Status init(StringData path, uint64_t bitMask);
    Status init(StringData path, const char* bitMaskBinary, uint32_t bitMaskLen);

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

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
        invariantOK(clone->init(path(), _bitPositions));
        if (getTag()) {
            clone->setTag(getTag()->clone());
        }
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

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
